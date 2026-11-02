#include "core/CloudCrypto.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

namespace CloudCrypto
{

static const QByteArray Magic("NXS1");
static constexpr int NonceLength = 12;
static constexpr int TagLength = 16;
static constexpr int SaltLength = 16;
static constexpr int RecoveryKeyLength = 32;
// Refuse KDF parameters from a (malicious) header that would freeze the app:
// 4x the memory and 3x the passes of what NixSCP itself writes
static constexpr quint32 MaxMemoryKib = 256 * 1024;
static constexpr quint32 MaxIterations = 10;
static constexpr quint32 MaxLanes = 8;
static constexpr int MaxHeaderLength = 64 * 1024;

static void SetError(QString * Error, const QString & Message)
{
  if (Error != nullptr)
  {
    *Error = Message;
  }
}

static void Wipe(QByteArray & Data)
{
  if (!Data.isEmpty())
  {
    OPENSSL_cleanse(Data.data(), static_cast<size_t>(Data.size()));
  }
  Data.clear();
}

QByteArray RandomBytes(int Length)
{
  QByteArray Result(Length, '\0');
  if (RAND_bytes(reinterpret_cast<unsigned char *>(Result.data()), Length) != 1)
  {
    qFatal("RAND_bytes failed");
  }
  return Result;
}

QByteArray DeriveKey(const QString & Password, const KdfParams & Params, QString * Error)
{
  if ((Params.MemoryKib < 8 * Params.Lanes) || (Params.MemoryKib > MaxMemoryKib) ||
      (Params.Iterations < 1) || (Params.Iterations > MaxIterations) ||
      (Params.Lanes < 1) || (Params.Lanes > MaxLanes) || (Params.Salt.size() < 8))
  {
    SetError(Error, QStringLiteral("Invalid key derivation parameters."));
    return QByteArray();
  }
  EVP_KDF * Kdf = EVP_KDF_fetch(nullptr, "ARGON2ID", nullptr);
  if (Kdf == nullptr)
  {
    SetError(Error, QStringLiteral("Argon2id is not available in OpenSSL (version 3.2 or newer is required)."));
    return QByteArray();
  }
  EVP_KDF_CTX * Ctx = EVP_KDF_CTX_new(Kdf);
  EVP_KDF_free(Kdf);
  if (Ctx == nullptr)
  {
    SetError(Error, QStringLiteral("Cannot create Argon2 context."));
    return QByteArray();
  }
  QByteArray Utf = Password.normalized(QString::NormalizationForm_KC).toUtf8();
  static char Empty = '\0';
  uint32_t Iterations = Params.Iterations;
  uint32_t Lanes = Params.Lanes;
  uint32_t Memory = Params.MemoryKib;
  // Single threaded: more threads would need OSSL_set_max_threads()
  uint32_t Threads = 1;
  uint32_t Version = 0x13;
  OSSL_PARAM OsslParams[] =
  {
    OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_PASSWORD,
      Utf.isEmpty() ? &Empty : Utf.data(), static_cast<size_t>(Utf.size())),
    OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT,
      const_cast<char *>(Params.Salt.constData()), static_cast<size_t>(Params.Salt.size())),
    OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ITER, &Iterations),
    OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_LANES, &Lanes),
    OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_MEMCOST, &Memory),
    OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_THREADS, &Threads),
    OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_VERSION, &Version),
    OSSL_PARAM_construct_end()
  };
  QByteArray Result(KeyLength, '\0');
  bool Ok = (EVP_KDF_derive(Ctx, reinterpret_cast<unsigned char *>(Result.data()), KeyLength, OsslParams) == 1);
  EVP_KDF_CTX_free(Ctx);
  Wipe(Utf);
  if (!Ok)
  {
    Wipe(Result);
    SetError(Error, QStringLiteral("Argon2 key derivation failed."));
  }
  return Result;
}

QByteArray Seal(const QByteArray & Key, const QByteArray & Plaintext, const QByteArray & Aad)
{
  Q_ASSERT(Key.size() == KeyLength);
  QByteArray Nonce = RandomBytes(NonceLength);
  QByteArray Result = Nonce;
  Result.resize(NonceLength + Plaintext.size() + TagLength);
  auto * Out = reinterpret_cast<unsigned char *>(Result.data()) + NonceLength;
  EVP_CIPHER_CTX * Ctx = EVP_CIPHER_CTX_new();
  int Len = 0;
  bool Ok = (Ctx != nullptr) &&
    (EVP_EncryptInit_ex(Ctx, EVP_aes_256_gcm(), nullptr,
      reinterpret_cast<const unsigned char *>(Key.constData()), reinterpret_cast<const unsigned char *>(Nonce.constData())) == 1) &&
    (Aad.isEmpty() || (EVP_EncryptUpdate(Ctx, nullptr, &Len, reinterpret_cast<const unsigned char *>(Aad.constData()), static_cast<int>(Aad.size())) == 1)) &&
    (EVP_EncryptUpdate(Ctx, Out, &Len, reinterpret_cast<const unsigned char *>(Plaintext.constData()), static_cast<int>(Plaintext.size())) == 1) &&
    (EVP_EncryptFinal_ex(Ctx, Out + Len, &Len) == 1) &&
    (EVP_CIPHER_CTX_ctrl(Ctx, EVP_CTRL_GCM_GET_TAG, TagLength, Out + Plaintext.size()) == 1);
  EVP_CIPHER_CTX_free(Ctx);
  if (!Ok)
  {
    qFatal("AES-256-GCM encryption failed");
  }
  return Result;
}

bool Open(const QByteArray & Key, const QByteArray & Sealed, const QByteArray & Aad, QByteArray & Plaintext)
{
  Plaintext.clear();
  if ((Key.size() != KeyLength) || (Sealed.size() < NonceLength + TagLength))
  {
    return false;
  }
  const auto * In = reinterpret_cast<const unsigned char *>(Sealed.constData());
  int CipherLength = static_cast<int>(Sealed.size()) - NonceLength - TagLength;
  QByteArray Tag = Sealed.right(TagLength);
  QByteArray Result(CipherLength, '\0');
  auto * Out = reinterpret_cast<unsigned char *>(Result.data());
  EVP_CIPHER_CTX * Ctx = EVP_CIPHER_CTX_new();
  int Len = 0;
  bool Ok = (Ctx != nullptr) &&
    (EVP_DecryptInit_ex(Ctx, EVP_aes_256_gcm(), nullptr, reinterpret_cast<const unsigned char *>(Key.constData()), In) == 1) &&
    (Aad.isEmpty() || (EVP_DecryptUpdate(Ctx, nullptr, &Len, reinterpret_cast<const unsigned char *>(Aad.constData()), static_cast<int>(Aad.size())) == 1)) &&
    (EVP_DecryptUpdate(Ctx, Out, &Len, In + NonceLength, CipherLength) == 1) &&
    (EVP_CIPHER_CTX_ctrl(Ctx, EVP_CTRL_GCM_SET_TAG, TagLength, Tag.data()) == 1) &&
    (EVP_DecryptFinal_ex(Ctx, Out + Len, &Len) == 1);
  EVP_CIPHER_CTX_free(Ctx);
  if (!Ok)
  {
    Wipe(Result);
    return false;
  }
  Plaintext = Result;
  return true;
}

static QByteArray Hkdf(const QByteArray & Ikm, const QByteArray & Info)
{
  EVP_KDF * Kdf = EVP_KDF_fetch(nullptr, "HKDF", nullptr);
  EVP_KDF_CTX * Ctx = (Kdf != nullptr) ? EVP_KDF_CTX_new(Kdf) : nullptr;
  EVP_KDF_free(Kdf);
  char Digest[] = "SHA256";
  OSSL_PARAM Params[] =
  {
    OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, Digest, 0),
    OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY, const_cast<char *>(Ikm.constData()), static_cast<size_t>(Ikm.size())),
    OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO, const_cast<char *>(Info.constData()), static_cast<size_t>(Info.size())),
    OSSL_PARAM_construct_end()
  };
  QByteArray Result(KeyLength, '\0');
  bool Ok = (Ctx != nullptr) && (EVP_KDF_derive(Ctx, reinterpret_cast<unsigned char *>(Result.data()), KeyLength, Params) == 1);
  EVP_KDF_CTX_free(Ctx);
  if (!Ok)
  {
    qFatal("HKDF failed");
  }
  return Result;
}

// The wraps are bound to the account: a header copied from another user's
// snapshot does not unwrap, even with the same password
static QByteArray WrapAad(const SnapshotHeader & Header)
{
  return QByteArrayLiteral("NXS1-DEK|") + Header.User.toUtf8();
}

static const char CrockfordAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

QByteArray NewRecoveryKey()
{
  return RandomBytes(RecoveryKeyLength);
}

QString FormatRecoveryKey(const QByteArray & Key)
{
  // 256 bits -> 52 base32 digits (the last one carries 1 bit + 4 zero bits)
  QString Digits;
  quint32 Buffer = 0;
  int Bits = 0;
  for (char Byte : Key)
  {
    Buffer = (Buffer << 8) | static_cast<quint8>(Byte);
    Bits += 8;
    while (Bits >= 5)
    {
      Digits += QLatin1Char(CrockfordAlphabet[(Buffer >> (Bits - 5)) & 0x1F]);
      Bits -= 5;
    }
  }
  if (Bits > 0)
  {
    Digits += QLatin1Char(CrockfordAlphabet[(Buffer << (5 - Bits)) & 0x1F]);
  }
  QString Result;
  for (int Index = 0; Index < Digits.size(); Index += 4)
  {
    if (!Result.isEmpty())
    {
      Result += QLatin1Char('-');
    }
    Result += Digits.mid(Index, 4);
  }
  return Result;
}

QByteArray ParseRecoveryKey(const QString & Text)
{
  QByteArray Result;
  quint32 Buffer = 0;
  int Bits = 0;
  int Count = 0;
  for (QChar C : Text.toUpper())
  {
    if (C.isSpace() || (C == QLatin1Char('-')))
    {
      continue;
    }
    if (C == QLatin1Char('O'))
    {
      C = QLatin1Char('0');
    }
    else if ((C == QLatin1Char('I')) || (C == QLatin1Char('L')))
    {
      C = QLatin1Char('1');
    }
    qsizetype Value = (C.unicode() < 128) ? QByteArray(CrockfordAlphabet).indexOf(C.toLatin1()) : -1;
    if (Value < 0)
    {
      return QByteArray();
    }
    Buffer = (Buffer << 5) | static_cast<quint32>(Value);
    Bits += 5;
    Count++;
    if (Bits >= 8)
    {
      if (Result.size() < RecoveryKeyLength)
      {
        Result += static_cast<char>((Buffer >> (Bits - 8)) & 0xFF);
      }
      Bits -= 8;
    }
  }
  return ((Count == 52) && (Result.size() == RecoveryKeyLength)) ? Result : QByteArray();
}

static QByteArray RecoveryWrapKey(const QByteArray & RecoveryKey)
{
  return Hkdf(RecoveryKey, QByteArrayLiteral("nixscp recovery key v1"));
}

bool WrapDek(SnapshotHeader & Header, const QByteArray & Dek, const QString & Password,
  const QByteArray & RecoveryKey, QString * Error)
{
  Header.Kdf.Salt = RandomBytes(SaltLength);
  QByteArray Kek = DeriveKey(Password, Header.Kdf, Error);
  if (Kek.isEmpty())
  {
    return false;
  }
  Header.DekPassword = Seal(Kek, Dek, WrapAad(Header));
  Wipe(Kek);
  if (!RecoveryKey.isEmpty())
  {
    QByteArray RecoveryKek = RecoveryWrapKey(RecoveryKey);
    Header.DekRecovery = Seal(RecoveryKek, Dek, WrapAad(Header));
    Wipe(RecoveryKek);
  }
  else
  {
    Header.DekRecovery.clear();
  }
  return true;
}

QByteArray UnwrapDekWithPassword(const SnapshotHeader & Header, const QString & Password, QString * Error)
{
  QByteArray Kek = DeriveKey(Password, Header.Kdf, Error);
  if (Kek.isEmpty())
  {
    return QByteArray();
  }
  QByteArray Dek;
  bool Ok = Open(Kek, Header.DekPassword, WrapAad(Header), Dek);
  Wipe(Kek);
  if (!Ok || (Dek.size() != KeyLength))
  {
    SetError(Error, QStringLiteral("The sync password is not correct."));
    return QByteArray();
  }
  return Dek;
}

QByteArray UnwrapDekWithRecoveryKey(const SnapshotHeader & Header, const QByteArray & RecoveryKey)
{
  if (Header.DekRecovery.isEmpty() || (RecoveryKey.size() != RecoveryKeyLength))
  {
    return QByteArray();
  }
  QByteArray RecoveryKek = RecoveryWrapKey(RecoveryKey);
  QByteArray Dek;
  bool Ok = Open(RecoveryKek, Header.DekRecovery, WrapAad(Header), Dek);
  Wipe(RecoveryKek);
  return (Ok && (Dek.size() == KeyLength)) ? Dek : QByteArray();
}

static QByteArray HeaderJson(const SnapshotHeader & Header)
{
  QJsonObject Kdf
  {
    {QStringLiteral("alg"), QStringLiteral("argon2id")},
    {QStringLiteral("m_kib"), static_cast<qint64>(Header.Kdf.MemoryKib)},
    {QStringLiteral("t"), static_cast<qint64>(Header.Kdf.Iterations)},
    {QStringLiteral("p"), static_cast<qint64>(Header.Kdf.Lanes)},
    {QStringLiteral("salt"), QString::fromLatin1(Header.Kdf.Salt.toBase64())},
  };
  QJsonObject Json
  {
    {QStringLiteral("fmt"), Header.Format},
    {QStringLiteral("slot"), Header.Slot},
    {QStringLiteral("user"), Header.User},
    {QStringLiteral("version"), Header.Version},
    {QStringLiteral("saved_at"), Header.SavedAt.toUTC().toString(Qt::ISODateWithMs)},
    {QStringLiteral("kdf"), Kdf},
    {QStringLiteral("dek_pw"), QString::fromLatin1(Header.DekPassword.toBase64())},
    {QStringLiteral("device"), Header.Device},
  };
  if (!Header.DekRecovery.isEmpty())
  {
    Json.insert(QStringLiteral("dek_rec"), QString::fromLatin1(Header.DekRecovery.toBase64()));
  }
  return QJsonDocument(Json).toJson(QJsonDocument::Compact);
}

QByteArray EncodeSnapshot(const SnapshotHeader & Header, const QByteArray & Dek, const QByteArray & Plaintext)
{
  QByteArray Json = HeaderJson(Header);
  QByteArray Prefix = Magic;
  char Length[4];
  qToLittleEndian<quint32>(static_cast<quint32>(Json.size()), Length);
  Prefix.append(Length, 4);
  Prefix.append(Json);
  return Prefix + Seal(Dek, qCompress(Plaintext, 9), Prefix);
}

// Splits the snapshot; Prefix = magic || length || header (the AAD)
static bool Split(const QByteArray & Snapshot, QByteArray & Prefix, QByteArray & Json, QByteArray & Sealed, QString * Error)
{
  if ((Snapshot.size() < 8) || !Snapshot.startsWith(Magic))
  {
    SetError(Error, QStringLiteral("Not a NixSCP sync snapshot."));
    return false;
  }
  quint32 Length = qFromLittleEndian<quint32>(Snapshot.constData() + 4);
  if ((Length == 0) || (Length > MaxHeaderLength) || (Snapshot.size() < 8 + static_cast<qsizetype>(Length) + NonceLength + TagLength))
  {
    SetError(Error, QStringLiteral("The sync snapshot is damaged."));
    return false;
  }
  Prefix = Snapshot.left(8 + Length);
  Json = Snapshot.mid(8, Length);
  Sealed = Snapshot.mid(8 + Length);
  return true;
}

bool ReadHeader(const QByteArray & Snapshot, SnapshotHeader & Header, QString * Error)
{
  QByteArray Prefix, Json, Sealed;
  if (!Split(Snapshot, Prefix, Json, Sealed, Error))
  {
    return false;
  }
  QJsonObject Obj = QJsonDocument::fromJson(Json).object();
  QJsonObject Kdf = Obj.value(QStringLiteral("kdf")).toObject();
  if ((Obj.value(QStringLiteral("fmt")).toInt() != 1) || (Kdf.value(QStringLiteral("alg")).toString() != QStringLiteral("argon2id")))
  {
    SetError(Error, QStringLiteral("The sync snapshot was made by a newer version of NixSCP."));
    return false;
  }
  Header = SnapshotHeader();
  Header.Format = 1;
  Header.Slot = Obj.value(QStringLiteral("slot")).toString();
  Header.User = Obj.value(QStringLiteral("user")).toString();
  Header.Version = Obj.value(QStringLiteral("version")).toInteger();
  Header.SavedAt = QDateTime::fromString(Obj.value(QStringLiteral("saved_at")).toString(), Qt::ISODateWithMs).toUTC();
  Header.Kdf.MemoryKib = static_cast<quint32>(Kdf.value(QStringLiteral("m_kib")).toInteger());
  Header.Kdf.Iterations = static_cast<quint32>(Kdf.value(QStringLiteral("t")).toInteger());
  Header.Kdf.Lanes = static_cast<quint32>(Kdf.value(QStringLiteral("p")).toInteger());
  Header.Kdf.Salt = QByteArray::fromBase64(Kdf.value(QStringLiteral("salt")).toString().toLatin1());
  Header.DekPassword = QByteArray::fromBase64(Obj.value(QStringLiteral("dek_pw")).toString().toLatin1());
  Header.DekRecovery = QByteArray::fromBase64(Obj.value(QStringLiteral("dek_rec")).toString().toLatin1());
  Header.Device = Obj.value(QStringLiteral("device")).toString();
  return true;
}

bool DecodeSnapshot(const QByteArray & Snapshot, const QByteArray & Dek, SnapshotHeader & Header,
  QByteArray & Plaintext, QString * Error)
{
  QByteArray Prefix, Json, Sealed;
  if (!Split(Snapshot, Prefix, Json, Sealed, Error) || !ReadHeader(Snapshot, Header, Error))
  {
    return false;
  }
  QByteArray Compressed;
  if (!Open(Dek, Sealed, Prefix, Compressed))
  {
    SetError(Error, QStringLiteral("The sync snapshot cannot be decrypted: the data key does not match or the data was modified."));
    return false;
  }
  Plaintext = qUncompress(Compressed);
  if (Plaintext.isEmpty() && !Compressed.isEmpty() && (Compressed.size() > 4) && (qFromBigEndian<quint32>(Compressed.constData()) != 0))
  {
    SetError(Error, QStringLiteral("The sync snapshot is damaged."));
    return false;
  }
  return true;
}

}
