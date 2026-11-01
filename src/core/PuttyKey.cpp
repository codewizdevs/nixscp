// PuTTY private key (.ppk) loader.
// Port of the PPK reading part of WinSCP source/putty/sshpubk.c
// (ppk_load_s, ssh2_ppk_derive_keys), with the key blob layouts of
// source/putty/crypto/rsa.c, crypto/dsa.c and crypto/ecc-ssh.c.
// The decrypted key is converted to an unencrypted "openssh-key-v1" private
// key and imported into libssh, as libssh cannot read .ppk files itself.
#include "core/PuttyKey.h"

#include <QFile>
#include <cctype>
#include <cstring>

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>

namespace PuttyKey
{

static const char PpkHeaderPrefix[] = "PuTTY-User-Key-File-";
static constexpr int MaxKeyFileSize = 1024 * 1024;
static constexpr int MaxKeyBlobLines = 400; // MAX_KEY_BLOB_LINES in sshpubk.c
static constexpr quint32 MaxArgon2Memory = 1024 * 1024; // KiB, i.e. 1 GiB

static void Wipe(QByteArray & Data)
{
  if (!Data.isEmpty())
  {
    // Buffers holding secrets are never implicitly shared (always deep
    // copies), so data() does not detach to a fresh copy here.
    OPENSSL_cleanse(Data.data(), static_cast<size_t>(Data.size()));
  }
  Data.clear();
}

// QByteArray that is wiped when it goes out of scope
class SecureBytes
{
public:
  SecureBytes() = default;
  SecureBytes(const SecureBytes &) = delete;
  SecureBytes & operator =(const SecureBytes &) = delete;
  ~SecureBytes() { Wipe(Data); }
  QByteArray Data;
};

// --- SSH binary encoding (marshal.c) ---

static void PutUint32(QByteArray & Buf, quint32 Value)
{
  Buf.append(static_cast<char>((Value >> 24) & 0xFF));
  Buf.append(static_cast<char>((Value >> 16) & 0xFF));
  Buf.append(static_cast<char>((Value >> 8) & 0xFF));
  Buf.append(static_cast<char>(Value & 0xFF));
}

static void PutString(QByteArray & Buf, const QByteArray & Data)
{
  PutUint32(Buf, static_cast<quint32>(Data.size()));
  Buf.append(Data);
}

static void PutString(QByteArray & Buf, const char * Str)
{
  PutString(Buf, QByteArray(Str));
}

// BinarySource equivalent
class BlobReader
{
public:
  explicit BlobReader(const QByteArray & AData) : FData(AData) {}

  quint32 GetUint32()
  {
    if (FError || (FData.size() - FPos < 4))
    {
      FError = true;
      return 0;
    }
    const unsigned char * P = reinterpret_cast<const unsigned char *>(FData.constData()) + FPos;
    FPos += 4;
    return (quint32(P[0]) << 24) | (quint32(P[1]) << 16) | (quint32(P[2]) << 8) | quint32(P[3]);
  }

  // Returns the string contents (a deep copy the caller must wipe if secret)
  QByteArray GetString()
  {
    quint32 Len = GetUint32();
    if (FError || (Len > static_cast<quint32>(FData.size() - FPos)))
    {
      FError = true;
      return QByteArray();
    }
    QByteArray Result(FData.constData() + FPos, static_cast<qsizetype>(Len));
    FPos += static_cast<int>(Len);
    return Result;
  }

  bool Error() const { return FError; }

private:
  const QByteArray & FData;
  int FPos = 0;
  bool FError = false;
};

// --- PPK text parsing (read_header / read_body / read_blob) ---

class PpkReader
{
public:
  explicit PpkReader(const QByteArray & AData) : FData(AData) {}

  // Reads "Name: Value" line, requiring the given Name
  bool ReadHeader(const char * Name, QByteArray & Value)
  {
    QByteArray Line;
    if (!ReadLine(Line))
    {
      return false;
    }
    int Colon = Line.indexOf(": ");
    if ((Colon <= 0) || (Colon > 39) || (Line.left(Colon) != Name))
    {
      return false;
    }
    Value = Line.mid(Colon + 2);
    return true;
  }

  bool ReadBlob(int Lines, QByteArray & Blob)
  {
    QByteArray Base64;
    for (int Index = 0; Index < Lines; Index++)
    {
      QByteArray Line;
      if (!ReadLine(Line) || (Line.size() % 4 != 0) || (Line.size() > 64))
      {
        Wipe(Base64);
        return false;
      }
      Base64.append(Line);
      Wipe(Line);
    }
    auto Decoded = QByteArray::fromBase64Encoding(Base64, QByteArray::AbortOnBase64DecodingErrors);
    Wipe(Base64);
    if (!Decoded)
    {
      return false;
    }
    Blob = std::move(Decoded.decoded);
    return true;
  }

private:
  bool ReadLine(QByteArray & Line)
  {
    if (FPos >= FData.size())
    {
      return false;
    }
    int End = FPos;
    while ((End < FData.size()) && (FData[End] != '\r') && (FData[End] != '\n'))
    {
      End++;
    }
    Line = FData.mid(FPos, End - FPos);
    // One line terminator: CR, LF, CRLF (or LFCR, as PuTTY accepts)
    if (End < FData.size())
    {
      char First = FData[End];
      End++;
      if ((End < FData.size()) && ((FData[End] == '\r') || (FData[End] == '\n')) && (FData[End] != First))
      {
        End++;
      }
    }
    FPos = End;
    return true;
  }

  const QByteArray & FData;
  int FPos = 0;
};

static bool ParseUint32(const QByteArray & Str, quint32 & Value)
{
  bool Ok = false;
  qulonglong Result = Str.toULongLong(&Ok, 10);
  if (!Ok || Str.isEmpty() || !std::isdigit(static_cast<unsigned char>(Str[0])) || (Result > 0xFFFFFFFFULL))
  {
    return false;
  }
  Value = static_cast<quint32>(Result);
  return true;
}

static bool ParseLineCount(const QByteArray & Str, int & Lines)
{
  quint32 Value = 0;
  if (!ParseUint32(Str, Value) || (Value >= static_cast<quint32>(MaxKeyBlobLines)))
  {
    return false;
  }
  Lines = static_cast<int>(Value);
  return true;
}

// --- Parsed (still encrypted) key file ---

struct PpkFile
{
  int Version = 0;
  QByteArray Algorithm;
  QByteArray Encryption;
  QByteArray Comment;
  QByteArray PublicBlob;
  QByteArray Kdf;
  quint32 Argon2Memory = 0;
  quint32 Argon2Passes = 0;
  quint32 Argon2Parallelism = 0;
  QByteArray Argon2Salt;
  QByteArray PrivateBlob;
  QByteArray Mac;

  bool Encrypted() const { return (Encryption == "aes256-cbc"); }

  ~PpkFile() { Wipe(PrivateBlob); }
};

static bool ParsePpk(const QByteArray & Data, PpkFile & File, QString & Error)
{
  PpkReader Reader(Data);
  QByteArray Value;

  // First line: "PuTTY-User-Key-File-<version>: <algorithm>"
  {
    int Colon = Data.indexOf(": ");
    int Eol = Data.indexOf('\n');
    if (!Data.startsWith(PpkHeaderPrefix) || (Colon < 0) || ((Eol >= 0) && (Colon > Eol)))
    {
      Error = QStringLiteral("Not a PuTTY SSH-2 private key.");
      return false;
    }
    QByteArray VersionStr = Data.mid(int(sizeof(PpkHeaderPrefix)) - 1, Colon - (int(sizeof(PpkHeaderPrefix)) - 1));
    if (VersionStr == "3")
    {
      File.Version = 3;
    }
    else if (VersionStr == "2")
    {
      File.Version = 2;
    }
    else if (VersionStr == "1")
    {
      Error = QStringLiteral("PuTTY key format version 1 is too old and not supported. "
        "Load and save the key with a current PuTTYgen.");
      return false;
    }
    else
    {
      Error = QStringLiteral("PuTTY key format too new.");
      return false;
    }
    QByteArray HeaderName = QByteArray(PpkHeaderPrefix) + VersionStr;
    if (!Reader.ReadHeader(HeaderName.constData(), File.Algorithm))
    {
      Error = QStringLiteral("Not a PuTTY SSH-2 private key.");
      return false;
    }
  }

  Error = QStringLiteral("PuTTY key file format error.");

  if (!Reader.ReadHeader("Encryption", File.Encryption) ||
      ((File.Encryption != "none") && (File.Encryption != "aes256-cbc")))
  {
    return false;
  }
  if (!Reader.ReadHeader("Comment", File.Comment))
  {
    return false;
  }
  int Lines = 0;
  if (!Reader.ReadHeader("Public-Lines", Value) || !ParseLineCount(Value, Lines) ||
      !Reader.ReadBlob(Lines, File.PublicBlob))
  {
    return false;
  }

  if ((File.Version >= 3) && File.Encrypted())
  {
    if (!Reader.ReadHeader("Key-Derivation", File.Kdf) ||
        ((File.Kdf != "Argon2id") && (File.Kdf != "Argon2i") && (File.Kdf != "Argon2d")))
    {
      return false;
    }
    if (!Reader.ReadHeader("Argon2-Memory", Value) || !ParseUint32(Value, File.Argon2Memory) ||
        !Reader.ReadHeader("Argon2-Passes", Value) || !ParseUint32(Value, File.Argon2Passes) ||
        !Reader.ReadHeader("Argon2-Parallelism", Value) || !ParseUint32(Value, File.Argon2Parallelism) ||
        !Reader.ReadHeader("Argon2-Salt", Value) || (Value.size() % 2 != 0))
    {
      return false;
    }
    for (char C : Value)
    {
      if (!std::isxdigit(static_cast<unsigned char>(C)))
      {
        return false;
      }
    }
    File.Argon2Salt = QByteArray::fromHex(Value);
    // argon2_params_bad()
    if ((File.Argon2Passes < 1) || (File.Argon2Parallelism < 1) || (File.Argon2Parallelism > 0xFFFFFF) ||
        (File.Argon2Memory < 8 * File.Argon2Parallelism) || (File.Argon2Salt.size() < 8))
    {
      Error = QStringLiteral("Invalid Argon2 parameters in PuTTY key file.");
      return false;
    }
    if (File.Argon2Memory > MaxArgon2Memory)
    {
      Error = QStringLiteral("Argon2 memory requirement of PuTTY key file is too high (%1 KiB).").arg(File.Argon2Memory);
      return false;
    }
  }

  if (!Reader.ReadHeader("Private-Lines", Value) || !ParseLineCount(Value, Lines) ||
      !Reader.ReadBlob(Lines, File.PrivateBlob))
  {
    return false;
  }
  if (!Reader.ReadHeader("Private-MAC", Value))
  {
    return false;
  }
  File.Mac = QByteArray::fromHex(Value);
  if ((File.Mac.size() != ((File.Version == 2) ? 20 : 32)) || (File.Mac.toHex() != Value.toLower()))
  {
    return false;
  }

  // The public blob must be of the algorithm named in the header
  BlobReader Public(File.PublicBlob);
  if (Public.GetString() != File.Algorithm)
  {
    return false;
  }

  Error.clear();
  return true;
}

// --- Key derivation, decryption and MAC (ssh2_ppk_derive_keys, ppk_load_s) ---

static bool Digest(const char * Name, const QByteArray & Data, QByteArray & Result)
{
  Result.resize(EVP_MAX_MD_SIZE);
  size_t Len = 0;
  if (EVP_Q_digest(nullptr, Name, nullptr, Data.constData(), static_cast<size_t>(Data.size()),
        reinterpret_cast<unsigned char *>(Result.data()), &Len) != 1)
  {
    return false;
  }
  Result.resize(static_cast<qsizetype>(Len));
  return true;
}

static bool Argon2(const PpkFile & File, const QByteArray & Passphrase, int Length, QByteArray & Result, QString & Error)
{
  const char * Name = (File.Kdf == "Argon2id") ? "ARGON2ID" : ((File.Kdf == "Argon2i") ? "ARGON2I" : "ARGON2D");
  EVP_KDF * Kdf = EVP_KDF_fetch(nullptr, Name, nullptr);
  if (Kdf == nullptr)
  {
    Error = QStringLiteral("%1 key derivation is not available in OpenSSL (version 3.2 or newer is required).")
      .arg(QString::fromLatin1(File.Kdf));
    return false;
  }
  EVP_KDF_CTX * Ctx = EVP_KDF_CTX_new(Kdf);
  EVP_KDF_free(Kdf);
  if (Ctx == nullptr)
  {
    Error = QStringLiteral("Cannot create Argon2 context.");
    return false;
  }

  // Pointers must be valid even for empty passphrase
  static char Empty = '\0';
  uint32_t Iterations = File.Argon2Passes;
  uint32_t Lanes = File.Argon2Parallelism;
  uint32_t Memory = File.Argon2Memory;
  // Single threaded: more threads would need OSSL_set_max_threads()
  uint32_t Threads = 1;
  uint32_t Version = 0x13;
  OSSL_PARAM Params[] =
  {
    OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_PASSWORD,
      Passphrase.isEmpty() ? &Empty : const_cast<char *>(Passphrase.constData()), static_cast<size_t>(Passphrase.size())),
    OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT,
      const_cast<char *>(File.Argon2Salt.constData()), static_cast<size_t>(File.Argon2Salt.size())),
    OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ITER, &Iterations),
    OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_LANES, &Lanes),
    OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_MEMCOST, &Memory),
    OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_THREADS, &Threads),
    OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_VERSION, &Version),
    OSSL_PARAM_construct_end()
  };
  Result.resize(Length);
  bool Ok = (EVP_KDF_derive(Ctx, reinterpret_cast<unsigned char *>(Result.data()), static_cast<size_t>(Length), Params) == 1);
  EVP_KDF_CTX_free(Ctx);
  if (!Ok)
  {
    Wipe(Result);
    Error = QStringLiteral("Argon2 key derivation failed.");
  }
  return Ok;
}

static bool DeriveKeys(const PpkFile & File, const QByteArray & Passphrase,
  QByteArray & CipherKey, QByteArray & CipherIV, QByteArray & MacKey, QString & Error)
{
  if (File.Version == 3)
  {
    if (!File.Encrypted())
    {
      // Unencrypted v3 keys use HMAC-SHA-256 with an empty key
      return true;
    }
    SecureBytes Output;
    if (!Argon2(File, Passphrase, 32 + 16 + 32, Output.Data, Error))
    {
      return false;
    }
    CipherKey = Output.Data.mid(0, 32);
    CipherIV = Output.Data.mid(32, 16);
    MacKey = Output.Data.mid(48, 32);
    return true;
  }
  else
  {
    if (File.Encrypted())
    {
      // Counter-mode iteration of SHA-1 to generate the cipher key; IV is all zero
      for (quint32 Counter = 0; Counter * 20 < 32; Counter++)
      {
        SecureBytes Input;
        PutUint32(Input.Data, Counter);
        Input.Data.append(Passphrase);
        SecureBytes Hash;
        if (!Digest("SHA1", Input.Data, Hash.Data))
        {
          Error = QStringLiteral("SHA-1 failed.");
          return false;
        }
        CipherKey.append(Hash.Data);
      }
      OPENSSL_cleanse(CipherKey.data() + 32, static_cast<size_t>(CipherKey.size() - 32));
      CipherKey.truncate(32);
      CipherIV = QByteArray(16, '\0');
    }
    // Completely separate hash for the MAC key
    SecureBytes Input;
    Input.Data = QByteArray("putty-private-key-file-mac-key");
    Input.Data.append(Passphrase);
    if (!Digest("SHA1", Input.Data, MacKey))
    {
      Error = QStringLiteral("SHA-1 failed.");
      return false;
    }
    return true;
  }
}

static bool DecryptAes256Cbc(QByteArray & Data, const QByteArray & Key, const QByteArray & IV)
{
  if ((Data.size() % 16 != 0) || (Key.size() != 32) || (IV.size() != 16))
  {
    return false;
  }
  if (Data.isEmpty())
  {
    return true;
  }
  EVP_CIPHER_CTX * Ctx = EVP_CIPHER_CTX_new();
  if (Ctx == nullptr)
  {
    return false;
  }
  SecureBytes Plain;
  Plain.Data.resize(Data.size() + 16);
  int Len1 = 0;
  int Len2 = 0;
  bool Ok =
    (EVP_DecryptInit_ex2(Ctx, EVP_aes_256_cbc(), reinterpret_cast<const unsigned char *>(Key.constData()),
       reinterpret_cast<const unsigned char *>(IV.constData()), nullptr) == 1) &&
    (EVP_CIPHER_CTX_set_padding(Ctx, 0) == 1) &&
    (EVP_DecryptUpdate(Ctx, reinterpret_cast<unsigned char *>(Plain.Data.data()), &Len1,
       reinterpret_cast<const unsigned char *>(Data.constData()), static_cast<int>(Data.size())) == 1) &&
    (EVP_DecryptFinal_ex(Ctx, reinterpret_cast<unsigned char *>(Plain.Data.data()) + Len1, &Len2) == 1) &&
    (Len1 + Len2 == Data.size());
  EVP_CIPHER_CTX_free(Ctx);
  if (Ok)
  {
    memcpy(Data.data(), Plain.Data.constData(), static_cast<size_t>(Data.size()));
  }
  return Ok;
}

static bool ComputeMac(const PpkFile & File, const QByteArray & MacKey, const QByteArray & PrivateBlob, QByteArray & Mac)
{
  SecureBytes MacData;
  PutString(MacData.Data, File.Algorithm);
  PutString(MacData.Data, File.Encryption);
  PutString(MacData.Data, File.Comment);
  PutString(MacData.Data, File.PublicBlob);
  PutString(MacData.Data, PrivateBlob);

  static unsigned char EmptyKey = 0;
  Mac.resize(EVP_MAX_MD_SIZE);
  size_t Len = 0;
  const unsigned char * Key = MacKey.isEmpty() ? &EmptyKey : reinterpret_cast<const unsigned char *>(MacKey.constData());
  unsigned char * Out = reinterpret_cast<unsigned char *>(Mac.data());
  if (EVP_Q_mac(nullptr, "HMAC", nullptr, (File.Version == 2) ? "SHA1" : "SHA256", nullptr,
        Key, static_cast<size_t>(MacKey.size()),
        reinterpret_cast<const unsigned char *>(MacData.Data.constData()), static_cast<size_t>(MacData.Data.size()),
        Out, static_cast<size_t>(Mac.size()), &Len) == nullptr)
  {
    return false;
  }
  Mac.resize(static_cast<qsizetype>(Len));
  return true;
}

enum DecryptResult { drOk, drWrongPassphrase, drError };

static DecryptResult Decrypt(const PpkFile & File, const QByteArray & Passphrase, QByteArray & PrivateBlob, QString & Error)
{
  SecureBytes CipherKey;
  SecureBytes CipherIV;
  SecureBytes MacKey;
  if (!DeriveKeys(File, Passphrase, CipherKey.Data, CipherIV.Data, MacKey.Data, Error))
  {
    return drError;
  }
  Wipe(PrivateBlob);
  PrivateBlob = QByteArray(File.PrivateBlob.constData(), File.PrivateBlob.size()); // deep copy
  if (File.Encrypted() && !DecryptAes256Cbc(PrivateBlob, CipherKey.Data, CipherIV.Data))
  {
    Wipe(PrivateBlob);
    Error = QStringLiteral("PuTTY key file format error.");
    return drError;
  }
  QByteArray Mac;
  if (!ComputeMac(File, MacKey.Data, PrivateBlob, Mac))
  {
    Wipe(PrivateBlob);
    Error = QStringLiteral("Cannot compute MAC of PuTTY key file.");
    return drError;
  }
  if ((Mac.size() != File.Mac.size()) ||
      (CRYPTO_memcmp(Mac.constData(), File.Mac.constData(), static_cast<size_t>(Mac.size())) != 0))
  {
    Wipe(PrivateBlob);
    // An incorrect MAC is an unconditional error if the key is unencrypted.
    // Otherwise, it means wrong passphrase.
    if (File.Encrypted())
    {
      return drWrongPassphrase;
    }
    Error = QStringLiteral("MAC failed: the PuTTY key file is corrupted.");
    return drError;
  }
  return drOk;
}

// --- Conversion to OpenSSH private key format (sshkey.c / PuTTY import.c) ---

static bool BuildOpenSshPrivateSection(const PpkFile & File, const QByteArray & PrivateBlob, QByteArray & Section,
  QString & Error)
{
  BlobReader Public(File.PublicBlob);
  BlobReader Private(PrivateBlob);
  // Raw string contents; mpints are copied in their SSH encoding unchanged
  SecureBytes F1, F2, F3, F4, F5, F6;
  QByteArray Algorithm = Public.GetString();
  PutString(Section, Algorithm);

  if (Algorithm == "ssh-rsa")
  {
    // public: e, n; private: d, p, q, iqmp (rsa_ssh1_..., rsa2_new_priv)
    // OpenSSH: n, e, d, iqmp, p, q
    QByteArray E = Public.GetString();
    QByteArray N = Public.GetString();
    F1.Data = Private.GetString(); // d
    F2.Data = Private.GetString(); // p
    F3.Data = Private.GetString(); // q
    F4.Data = Private.GetString(); // iqmp
    if (Public.Error() || Private.Error())
    {
      Error = QStringLiteral("Invalid RSA key data in PuTTY key file.");
      return false;
    }
    PutString(Section, N);
    PutString(Section, E);
    PutString(Section, F1.Data);
    PutString(Section, F4.Data);
    PutString(Section, F2.Data);
    PutString(Section, F3.Data);
  }
  else if (Algorithm == "ssh-dss")
  {
    // public: p, q, g, y; private: x. OpenSSH: p, q, g, y, x
    QByteArray P = Public.GetString();
    QByteArray Q = Public.GetString();
    QByteArray G = Public.GetString();
    QByteArray Y = Public.GetString();
    F1.Data = Private.GetString();
    if (Public.Error() || Private.Error())
    {
      Error = QStringLiteral("Invalid DSA key data in PuTTY key file.");
      return false;
    }
    PutString(Section, P);
    PutString(Section, Q);
    PutString(Section, G);
    PutString(Section, Y);
    PutString(Section, F1.Data);
  }
  else if ((Algorithm == "ecdsa-sha2-nistp256") || (Algorithm == "ecdsa-sha2-nistp384") ||
           (Algorithm == "ecdsa-sha2-nistp521"))
  {
    // public: curve name, point Q; private: mpint scalar (ecdsa_new_priv).
    // OpenSSH: curve name, Q, scalar
    QByteArray Curve = Public.GetString();
    QByteArray Point = Public.GetString();
    F1.Data = Private.GetString();
    if (Public.Error() || Private.Error() || (Algorithm.mid(11) != Curve))
    {
      Error = QStringLiteral("Invalid ECDSA key data in PuTTY key file.");
      return false;
    }
    PutString(Section, Curve);
    PutString(Section, Point);
    PutString(Section, F1.Data);
  }
  else if (Algorithm == "ssh-ed25519")
  {
    // public: 32 byte point; private: the 32 byte secret seed, stored as a
    // little endian integer string (eddsa_private_blob, put_mp_le_fixedlen).
    // OpenSSH: public key, seed || public key
    QByteArray PublicKey = Public.GetString();
    F1.Data = Private.GetString();
    if (Public.Error() || Private.Error() || (PublicKey.size() != 32) || (F1.Data.size() > 32))
    {
      Error = QStringLiteral("Invalid Ed25519 key data in PuTTY key file.");
      return false;
    }
    // A shorter little endian integer has implicit high order zero bytes
    F2.Data = F1.Data;
    F2.Data.append(QByteArray(32 - F1.Data.size(), '\0'));
    F2.Data.append(PublicKey);
    PutString(Section, PublicKey);
    PutString(Section, F2.Data);
  }
  else
  {
    Error = QStringLiteral("Key type '%1' is not supported.").arg(QString::fromLatin1(Algorithm));
    return false;
  }
  PutString(Section, File.Comment);
  return true;
}

static bool BuildOpenSshKey(const PpkFile & File, const QByteArray & PrivateBlob, QByteArray & Pem, QString & Error)
{
  SecureBytes Section;
  // Check integers (any value, but both equal)
  PutUint32(Section.Data, 0x4C696E53);
  PutUint32(Section.Data, 0x4C696E53);
  if (!BuildOpenSshPrivateSection(File, PrivateBlob, Section.Data, Error))
  {
    return false;
  }
  // Padding to the cipher block size (8 for "none")
  for (char Pad = 1; Section.Data.size() % 8 != 0; Pad++)
  {
    Section.Data.append(Pad);
  }

  SecureBytes Blob;
  Blob.Data.append("openssh-key-v1", 15); // including terminating NUL
  PutString(Blob.Data, "none"); // cipher
  PutString(Blob.Data, "none"); // kdf
  PutString(Blob.Data, QByteArray()); // kdf options
  PutUint32(Blob.Data, 1); // number of keys
  PutString(Blob.Data, File.PublicBlob);
  PutString(Blob.Data, Section.Data);

  SecureBytes Base64;
  Base64.Data = Blob.Data.toBase64();
  Pem = QByteArray("-----BEGIN OPENSSH PRIVATE KEY-----\n");
  for (int Pos = 0; Pos < Base64.Data.size(); Pos += 70)
  {
    Pem.append(Base64.Data.constData() + Pos, qMin<qsizetype>(70, Base64.Data.size() - Pos));
    Pem.append('\n');
  }
  Pem.append("-----END OPENSSH PRIVATE KEY-----\n");
  return true;
}

static bool ImportKey(const PpkFile & File, const QByteArray & PrivateBlob, ssh_key * Key, QString & Error)
{
  // libssh 0.11 is normally built without DSA (and then also leaks on the
  // failed import), so check up front
  if ((File.Algorithm == "ssh-dss") && (ssh_key_type_from_name("ssh-dss") != SSH_KEYTYPE_DSS))
  {
    Error = QStringLiteral("DSA keys are not supported by this build of libssh.");
    return false;
  }
  SecureBytes Pem;
  if (!BuildOpenSshKey(File, PrivateBlob, Pem.Data, Error))
  {
    return false;
  }
  ssh_key Result = nullptr;
  if ((ssh_pki_import_privkey_base64(Pem.Data.constData(), nullptr, nullptr, nullptr, &Result) != SSH_OK) ||
      (Result == nullptr))
  {
    Error = QStringLiteral("The key could not be imported into libssh.");
    return false;
  }
  *Key = Result;
  return true;
}

// --- Public API ---

bool IsPuttyKeyFile(const QString & FileName)
{
  QFile File(FileName);
  if (!File.open(QIODevice::ReadOnly))
  {
    return false;
  }
  QByteArray Head = File.read(int(sizeof(PpkHeaderPrefix)) - 1);
  return (Head == PpkHeaderPrefix);
}

LoadResult Load(const QString & FileName, const std::function<bool(QString & Passphrase, bool Retry)> & PromptPassphrase,
  ssh_key * Key, QString * Error)
{
  QString ErrorStr;
  auto Fail = [&](const QString & Message) -> LoadResult
  {
    if (Error != nullptr)
    {
      *Error = Message;
    }
    return lrError;
  };

  if (Key == nullptr)
  {
    return Fail(QStringLiteral("No key output given."));
  }
  *Key = nullptr;

  SecureBytes Data;
  {
    QFile File(FileName);
    if (!File.open(QIODevice::ReadOnly))
    {
      return Fail(QStringLiteral("Unable to open key file \"%1\": %2").arg(FileName, File.errorString()));
    }
    if (File.size() > MaxKeyFileSize)
    {
      return Fail(QStringLiteral("Key file \"%1\" is too large.").arg(FileName));
    }
    Data.Data = File.readAll();
  }

  PpkFile Ppk;
  if (!ParsePpk(Data.Data, Ppk, ErrorStr))
  {
    return Fail(ErrorStr);
  }
  Wipe(Data.Data);

  SecureBytes PrivateBlob;
  if (!Ppk.Encrypted())
  {
    if (Decrypt(Ppk, QByteArray(), PrivateBlob.Data, ErrorStr) != drOk)
    {
      return Fail(ErrorStr);
    }
  }
  else
  {
    if (!PromptPassphrase)
    {
      return Fail(QStringLiteral("Key is encrypted and no passphrase can be prompted for."));
    }
    bool Decrypted = false;
    for (int Attempt = 0; !Decrypted && (Attempt < 3); Attempt++)
    {
      QString Passphrase;
      if (!PromptPassphrase(Passphrase, (Attempt > 0)))
      {
        // Best effort wipe of the passphrase copy
        Passphrase.fill(QChar(0));
        if (Error != nullptr)
        {
          Error->clear();
        }
        return lrCancelled;
      }
      SecureBytes PassphraseUtf8;
      PassphraseUtf8.Data = Passphrase.toUtf8();
      Passphrase.fill(QChar(0));
      DecryptResult Result = Decrypt(Ppk, PassphraseUtf8.Data, PrivateBlob.Data, ErrorStr);
      if (Result == drError)
      {
        return Fail(ErrorStr);
      }
      Decrypted = (Result == drOk);
    }
    if (!Decrypted)
    {
      return Fail(QStringLiteral("Wrong passphrase"));
    }
  }

  if (!ImportKey(Ppk, PrivateBlob.Data, Key, ErrorStr))
  {
    return Fail(ErrorStr);
  }
  if (Error != nullptr)
  {
    Error->clear();
  }
  return lrOk;
}

}
