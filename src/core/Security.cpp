// Port of WinSCP source/core/Security.cpp, core/Cryptography.cpp (password
// manager part) and windows/WinConfiguration.cpp (master password)
#include "core/Security.h"

#include <QFile>
#include <QHash>
#include <QMutex>
#include <QRandomGenerator>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <cstring>
#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#endif
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
#include <sys/prctl.h>
#endif
#ifdef Q_OS_MACOS
#include <sys/types.h>
#include <sys/proc.h>
#include <sys/ptrace.h>
#include <sys/sysctl.h>
#endif

//---------------------------------------------------------------------------
// SecureBytes

#ifdef Q_OS_WIN
// A function of kernel32 that older versions of Windows do not have
static FARPROC Kernel32Function(const char * Name)
{
  return GetProcAddress(GetModuleHandleW(L"kernel32.dll"), Name);
}

// Left out of the dumps of Windows Error Reporting (Windows 10 1903 and newer)
static void ExcludeFromDumps(void * Memory, size_t Size)
{
  using Function = HRESULT (WINAPI *)(const void *, DWORD);
  static const auto Exclude = reinterpret_cast<Function>(reinterpret_cast<void *>(Kernel32Function("WerRegisterExcludedMemoryBlock")));
  if (Exclude != nullptr)
  {
    Exclude(Memory, static_cast<DWORD>(Size));
  }
}

static void IncludeInDumps(void * Memory)
{
  using Function = HRESULT (WINAPI *)(const void *);
  static const auto Include = reinterpret_cast<Function>(reinterpret_cast<void *>(Kernel32Function("WerUnregisterExcludedMemoryBlock")));
  if (Include != nullptr)
  {
    Include(Memory);
  }
}
#endif

// A secret is small, so a page of its own costs little and can be locked and
// excluded from dumps alone. Locking is best effort: the limit for locked
// memory is low on some systems, and the key is still wiped.
static char * AllocateSecret(size_t Size, size_t & Capacity)
{
#ifdef Q_OS_WIN
  SYSTEM_INFO Info;
  GetSystemInfo(&Info);
  size_t Page = Info.dwPageSize;
  Capacity = ((Size + Page - 1) / Page) * Page;
  void * Memory = VirtualAlloc(nullptr, Capacity, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (Memory == nullptr)
  {
    qFatal("Cannot allocate memory for a key");
  }
  VirtualLock(Memory, Capacity);
  ExcludeFromDumps(Memory, Capacity);
#else
  size_t Page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  Capacity = ((Size + Page - 1) / Page) * Page;
  void * Memory = mmap(nullptr, Capacity, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (Memory == MAP_FAILED)
  {
    qFatal("Cannot allocate memory for a key");
  }
  mlock(Memory, Capacity);
#ifdef MADV_DONTDUMP
  madvise(Memory, Capacity, MADV_DONTDUMP);
#endif
#endif
  return static_cast<char *>(Memory);
}

static void ReleaseSecret(char * Memory, size_t Capacity)
{
  OPENSSL_cleanse(Memory, Capacity);
#ifdef Q_OS_WIN
  IncludeInDumps(Memory);
  VirtualUnlock(Memory, Capacity);
  VirtualFree(Memory, 0, MEM_RELEASE);
#else
  munlock(Memory, Capacity);
  munmap(Memory, Capacity);
#endif
}

SecureBytes::SecureBytes(SecureBytes && Other) noexcept :
  FData(std::exchange(Other.FData, nullptr)),
  FSize(std::exchange(Other.FSize, 0)),
  FCapacity(std::exchange(Other.FCapacity, 0))
{
}

SecureBytes & SecureBytes::operator=(const SecureBytes & Other)
{
  if (this != &Other)
  {
    Assign(Other.View());
  }
  return *this;
}

SecureBytes & SecureBytes::operator=(SecureBytes && Other) noexcept
{
  if (this != &Other)
  {
    Clear();
    FData = std::exchange(Other.FData, nullptr);
    FSize = std::exchange(Other.FSize, 0);
    FCapacity = std::exchange(Other.FCapacity, 0);
  }
  return *this;
}

void SecureBytes::Assign(const QByteArray & Data)
{
  // Released last: Data may be a View of this object
  SecureBytes Old(std::move(*this));
  if (!Data.isEmpty())
  {
    FData = AllocateSecret(static_cast<size_t>(Data.size()), FCapacity);
    FSize = static_cast<int>(Data.size());
    std::memcpy(FData, Data.constData(), static_cast<size_t>(FSize));
  }
}

void SecureBytes::Clear()
{
  if (FData != nullptr)
  {
    ReleaseSecret(FData, FCapacity);
  }
  FData = nullptr;
  FSize = 0;
  FCapacity = 0;
}

QByteArray SecureBytes::View() const
{
  return (FData != nullptr) ? QByteArray::fromRawData(FData, FSize) : QByteArray();
}

QByteArray SecureBytes::Copy() const
{
  return (FData != nullptr) ? QByteArray(FData, FSize) : QByteArray();
}

bool SecureBytes::Equals(const QByteArray & Data) const
{
  return (Data.size() == FSize) &&
    ((FSize == 0) || (CRYPTO_memcmp(FData, Data.constData(), static_cast<size_t>(FSize)) == 0));
}

namespace Security
{

static const char PWALG_SIMPLE_STRING[] = "0123456789ABCDEF";

// xdg-desktop-portal identifies its caller through /proc/<pid>/root, which a
// non-dumpable process closes: every portal call is refused, so the file
// dialogs of a session using portals would fail without a message
bool DesktopPortalInUse()
{
  return qEnvironmentVariable("QT_QPA_PLATFORMTHEME").contains(QStringLiteral("portal"), Qt::CaseInsensitive) ||
    (qEnvironmentVariable("GTK_USE_PORTAL") == QStringLiteral("1")) || qEnvironmentVariableIsSet("SNAP") ||
    QFile::exists(QStringLiteral("/.flatpak-info"));
}

void ProtectProcessMemory()
{
#if defined(QT_NO_DEBUG) && !defined(Q_OS_ANDROID)
  static bool Done = false;
  if (Done || qEnvironmentVariableIsSet("NIXSCP_ALLOW_COREDUMP"))
  {
    return;
  }
  Done = true;
#if defined(Q_OS_WIN)
  // Attached before the key arrived: someone is debugging this process
  if (IsDebuggerPresent())
  {
    return;
  }
  // A crash ends the process without Windows Error Reporting, so also without
  // the dumps an administrator may have set up (LocalDumps). Programs started
  // from here inherit this. Reports of a hanging window are not covered, hence
  // no heap in them, and SecureBytes is left out of dumps by itself.
  SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX);
  using SetFlags = HRESULT (WINAPI *)(DWORD);
  auto WerFlags = reinterpret_cast<SetFlags>(reinterpret_cast<void *>(Kernel32Function("WerSetFlags")));
  if (WerFlags != nullptr)
  {
    constexpr DWORD NoHeap = 1;   // WER_FAULT_REPORTING_FLAG_NOHEAP
    WerFlags(NoHeap);
  }
#else
#if defined(Q_OS_LINUX)
  // Attached before the key arrived: someone is debugging this process
  QFile Status(QStringLiteral("/proc/self/status"));
  if (Status.open(QIODevice::ReadOnly) && !Status.readAll().contains("TracerPid:\t0\n"))
  {
    return;
  }
#elif defined(Q_OS_MACOS)
  // The same; PT_DENY_ATTACH would end a process that is being traced
  struct kinfo_proc Process = {};
  size_t ProcessSize = sizeof(Process);
  int Request[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
  if ((sysctl(Request, 4, &Process, &ProcessSize, nullptr, 0) != 0) || ((Process.kp_proc.p_flag & P_TRACED) != 0))
  {
    return;
  }
#endif
  // The soft limit only: programs started from here may still raise theirs
  struct rlimit Limit;
  if (getrlimit(RLIMIT_CORE, &Limit) == 0)
  {
    Limit.rlim_cur = 0;
    setrlimit(RLIMIT_CORE, &Limit);
  }
#if defined(Q_OS_LINUX)
  if (!DesktopPortalInUse())
  {
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
  }
#elif defined(Q_OS_MACOS)
  // No debugger of this user from now on. Unsigned, a program started with
  // DYLD_INSERT_LIBRARIES or under a debugger gets around this
  ptrace(PT_DENY_ATTACH, 0, nullptr, 0);
#endif
#endif
#endif
}

// Overwrites a buffer holding a secret (plain or scrambled password, key
// material) before it is released. Only an unshared buffer is overwritten:
// data() of a shared QByteArray would detach and wipe a fresh copy only.
void WipeBytes(QByteArray & Data)
{
  if (!Data.isEmpty() && !Data.isDetached())
  {
    Data.clear();
    return;
  }
  if (!Data.isEmpty())
  {
    OPENSSL_cleanse(Data.data(), static_cast<size_t>(Data.size()));
  }
  Data.clear();
}
static constexpr unsigned char PWALG_SIMPLE_INTERNAL = 0x00;
static constexpr unsigned char PWALG_SIMPLE_EXTERNAL = 0x01;
static constexpr unsigned char PWALG_SIMPLE_INTERNAL2 = 0x02;

static QByteArray SimpleEncryptChar(unsigned char Ch)
{
  Ch = static_cast<unsigned char>((~Ch) ^ PWALG_SIMPLE_MAGIC);
  QByteArray Result(2, '0');
  Result[0] = PWALG_SIMPLE_STRING[(Ch & 0xF0) >> 4];
  Result[1] = PWALG_SIMPLE_STRING[(Ch & 0x0F) >> 0];
  return Result;
}

static int HexPos(char C)
{
  const char * P = strchr(PWALG_SIMPLE_STRING, C);
  return ((P != nullptr) && (C != '\0')) ? static_cast<int>(P - PWALG_SIMPLE_STRING) : -1;
}

static unsigned char SimpleDecryptNextChar(QByteArray & Str)
{
  if (Str.size() >= 2)
  {
    // As in WinSCP, an invalid character yields Pos()==0, i.e. -1 here
    unsigned char Result = static_cast<unsigned char>(
      ~((((HexPos(Str[0])) << 4) + ((HexPos(Str[1])) << 0)) ^ PWALG_SIMPLE_MAGIC));
    Str.remove(0, 2);
    return Result;
  }
  else
  {
    Str.clear();
    return 0x00;
  }
}

QByteArray SimpleEncryptPassword(const QString & UnicodePassword, const QString & UnicodeKey)
{
  QByteArray Password = UnicodeKey.toUtf8() + UnicodePassword.toUtf8();
  QByteArray Result;
  auto Random = QRandomGenerator::global();

  Result += SimpleEncryptChar(PWALG_SIMPLE_FLAG);
  int Len = Password.size();
  if (Len > 255)
  {
    Result += SimpleEncryptChar(PWALG_SIMPLE_INTERNAL2);
    Result += SimpleEncryptChar(static_cast<unsigned char>(Len >> 8));
    Result += SimpleEncryptChar(static_cast<unsigned char>(Len & 0xFF));
  }
  else
  {
    Result += SimpleEncryptChar(PWALG_SIMPLE_INTERNAL);
    Result += SimpleEncryptChar(static_cast<unsigned char>(Len));
  }
  int DataLen = (Result.size() / 2) + 1 + Password.size();
  int Shift = (DataLen < PWALG_SIMPLE_MAXLEN) ? Random->bounded(PWALG_SIMPLE_MAXLEN - DataLen) : 0;
  Result += SimpleEncryptChar(static_cast<unsigned char>(Shift));
  for (int Index = 0; Index < Shift; Index++)
  {
    Result += SimpleEncryptChar(static_cast<unsigned char>(Random->bounded(256)));
  }
  for (int Index = 0; Index < Password.size(); Index++)
  {
    Result += SimpleEncryptChar(static_cast<unsigned char>(Password[Index]));
  }
  while (Result.size() < PWALG_SIMPLE_MAXLEN * 2)
  {
    Result += SimpleEncryptChar(static_cast<unsigned char>(Random->bounded(256)));
  }
  return Result;
}

// Returns the raw UTF-8 content; CheckKey = false keeps the key prefix
static QByteArray SimpleDecrypt(const QByteArray & APassword, const QByteArray & Key, bool CheckKey)
{
  QByteArray Password = APassword;
  int Length;
  unsigned char Flag = SimpleDecryptNextChar(Password);
  if (Flag == PWALG_SIMPLE_FLAG)
  {
    unsigned char Version = SimpleDecryptNextChar(Password);
    if (Version == PWALG_SIMPLE_INTERNAL)
    {
      Length = SimpleDecryptNextChar(Password);
    }
    else if (Version == PWALG_SIMPLE_INTERNAL2)
    {
      int Hi = SimpleDecryptNextChar(Password);
      int Lo = SimpleDecryptNextChar(Password);
      Length = (Hi << 8) + Lo;
    }
    else
    {
      Length = -1;
    }
  }
  else
  {
    Length = Flag;
  }

  QByteArray Result;
  if (Length >= 0)
  {
    int Skip = static_cast<int>(SimpleDecryptNextChar(Password)) * 2;
    Password.remove(0, Skip);
    for (int Index = 0; Index < Length; Index++)
    {
      Result += static_cast<char>(SimpleDecryptNextChar(Password));
    }
    if ((Flag == PWALG_SIMPLE_FLAG) && CheckKey)
    {
      if (!Result.startsWith(Key))
      {
        Result.clear();
      }
      else
      {
        Result.remove(0, Key.size());
      }
    }
  }
  return Result;
}

QString SimpleDecryptPassword(const QByteArray & Password, const QString & UnicodeKey)
{
  return QString::fromUtf8(SimpleDecrypt(Password, UnicodeKey.toUtf8(), true));
}

QByteArray SetExternalEncryptedPassword(const QByteArray & Password)
{
  QByteArray Result;
  Result += SimpleEncryptChar(PWALG_SIMPLE_FLAG);
  Result += SimpleEncryptChar(PWALG_SIMPLE_EXTERNAL);
  Result += BytesToHex(Password).toLatin1();
  return Result;
}

bool GetExternalEncryptedPassword(const QByteArray & AEncrypted, QByteArray & Password)
{
  QByteArray Encrypted = AEncrypted;
  bool Result =
    (SimpleDecryptNextChar(Encrypted) == PWALG_SIMPLE_FLAG) &&
    (SimpleDecryptNextChar(Encrypted) == PWALG_SIMPLE_EXTERNAL);
  if (Result)
  {
    Password = HexToBytes(QString::fromLatin1(Encrypted));
  }
  return Result;
}

bool IsStronglyEncrypted(const QByteArray & Password)
{
  QByteArray Dummy;
  return GetExternalEncryptedPassword(Password, Dummy);
}

QString BytesToHex(const QByteArray & Bytes)
{
  return QString::fromLatin1(Bytes.toHex().toUpper());
}

QByteArray HexToBytes(const QString & Hex)
{
  // core/Common.cpp HexToBytes: empty result on any invalid digit or odd length
  static const QString Digits = QStringLiteral("0123456789ABCDEF");
  QByteArray Result;
  if ((Hex.size() % 2) == 0)
  {
    for (int I = 0; I < Hex.size(); I += 2)
    {
      int P1 = Digits.indexOf(Hex[I].toUpper());
      int P2 = Digits.indexOf(Hex[I + 1].toUpper());
      if ((P1 < 0) || (P2 < 0))
      {
        return QByteArray();
      }
      Result += static_cast<char>((P1 << 4) + P2);
    }
  }
  return Result;
}

//---------------------------------------------------------------------------
// core/Cryptography.cpp: fcrypt (WinZip AES compatible, mode 3 = AES-256)
static constexpr int BlockSize = 16;
static constexpr int KeyLength = 32;        // KEY_LENGTH(3)
static constexpr int SaltLength = 16;       // SALT_LENGTH(3)
static constexpr int MacLength = 10;        // MAC_LENGTH(3)
static constexpr int KeyingIterations = 1000;
static constexpr int PwdVerLength = 2;

namespace
{
struct FCrypt
{
  unsigned char Nonce[BlockSize];
  unsigned char EncrBuffer[BlockSize];
  unsigned int EncrPos;
  unsigned char AuthKey[KeyLength];
  EVP_CIPHER_CTX * Aes;

  FCrypt(const QString & Password, const QByteArray & Salt)
  {
    // fcrypt_init: derive the encryption and authentication keys
    // (and the unused password verifier) by PBKDF2-HMAC-SHA1
    QByteArray UtfPassword = Password.toUtf8();
    unsigned char KBuf[2 * KeyLength + PwdVerLength];
    PKCS5_PBKDF2_HMAC(UtfPassword.constData(), UtfPassword.size(),
      reinterpret_cast<const unsigned char *>(Salt.constData()), Salt.size(),
      KeyingIterations, EVP_sha1(), sizeof(KBuf), KBuf);
    WipeBytes(UtfPassword);
    memset(Nonce, 0, sizeof(Nonce));
    EncrPos = BlockSize;
    Aes = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(Aes, EVP_aes_256_ecb(), nullptr, KBuf, nullptr);
    EVP_CIPHER_CTX_set_padding(Aes, 0);
    memcpy(AuthKey, KBuf + KeyLength, KeyLength);
    OPENSSL_cleanse(KBuf, sizeof(KBuf));
  }

  ~FCrypt()
  {
    EVP_CIPHER_CTX_free(Aes);
    OPENSSL_cleanse(AuthKey, sizeof(AuthKey));
    OPENSSL_cleanse(EncrBuffer, sizeof(EncrBuffer));
    OPENSSL_cleanse(Nonce, sizeof(Nonce));
  }

  FCrypt(const FCrypt &) = delete;
  FCrypt & operator =(const FCrypt &) = delete;

  // encr_data: CTR mode with a little-endian 64-bit counter in the nonce
  void EncrData(unsigned char * Data, int Length)
  {
    for (int I = 0; I < Length; I++)
    {
      if (EncrPos == BlockSize)
      {
        int J = 0;
        while ((J < 8) && !++Nonce[J])
        {
          ++J;
        }
        int OutLen = 0;
        EVP_EncryptUpdate(Aes, EncrBuffer, &OutLen, Nonce, BlockSize);
        EncrPos = 0;
      }
      Data[I] ^= EncrBuffer[EncrPos++];
    }
  }

  QByteArray Mac(const QByteArray & Data) const
  {
    unsigned char Digest[EVP_MAX_MD_SIZE];
    unsigned int DigestLen = 0;
    HMAC(EVP_sha1(), AuthKey, KeyLength,
      reinterpret_cast<const unsigned char *>(Data.constData()), Data.size(), Digest, &DigestLen);
    QByteArray Result(reinterpret_cast<const char *>(Digest), MacLength);
    OPENSSL_cleanse(Digest, sizeof(Digest));
    return Result;
  }
};

// MAC comparison in constant time
bool SameMac(const QByteArray & Mac1, const QByteArray & Mac2)
{
  return (Mac1.size() == Mac2.size()) &&
    (CRYPTO_memcmp(Mac1.constData(), Mac2.constData(), static_cast<size_t>(Mac1.size())) == 0);
}
}

static QByteArray AES256Salt()
{
  QByteArray Salt(SaltLength, '\0');
  if (RAND_bytes(reinterpret_cast<unsigned char *>(Salt.data()), Salt.size()) != 1)
  {
    // never an all-zero (predictable) salt
    QRandomGenerator::system()->generate(Salt.begin(), Salt.end());
  }
  return Salt;
}

static void AES256EncryptWithMAC(const QByteArray & Input, const QString & Password,
  QByteArray & Salt, QByteArray & Output, QByteArray & Mac)
{
  if (Salt.isEmpty())
  {
    Salt = AES256Salt();
  }
  FCrypt Crypt(Password, Salt);
  Output = Input;
  Crypt.EncrData(reinterpret_cast<unsigned char *>(Output.data()), Output.size());
  Mac = Crypt.Mac(Output);
}

void AES256EncryptWithMAC(const QByteArray & Input, const QString & Password, QByteArray & Output)
{
  QByteArray Salt;
  QByteArray Encrypted;
  QByteArray Mac;
  AES256EncryptWithMAC(Input, Password, Salt, Encrypted, Mac);
  Output = Salt + Encrypted + Mac;
}

bool AES256DecryptWithMAC(const QByteArray & Input, const QString & Password, QByteArray & Output)
{
  bool Result = (Input.size() > SaltLength + MacLength);
  if (Result)
  {
    QByteArray Salt = Input.left(SaltLength);
    QByteArray Encrypted = Input.mid(SaltLength, Input.size() - SaltLength - MacLength);
    QByteArray Mac = Input.right(MacLength);
    FCrypt Crypt(Password, Salt);
    // fcrypt_decrypt: authenticate first, then decrypt (nothing is decrypted
    // from data that does not authenticate)
    QByteArray Mac2 = Crypt.Mac(Encrypted);
    Result = SameMac(Mac, Mac2);
    if (Result)
    {
      WipeBytes(Output);
      Output = Encrypted;
      Crypt.EncrData(reinterpret_cast<unsigned char *>(Output.data()), Output.size());
    }
  }
  return Result;
}

void AES256CreateVerifier(const QString & Input, QByteArray & Verifier)
{
  QByteArray Salt;
  QByteArray Dummy = AES256Salt();
  QByteArray Encrypted;
  QByteArray Mac;
  AES256EncryptWithMAC(Dummy, Input, Salt, Encrypted, Mac);
  Verifier = Salt + Dummy + Mac;
}

bool AES256Verify(const QString & Input, const QByteArray & Verifier)
{
  if (Verifier.size() < SaltLength + SaltLength + MacLength)
  {
    return false;
  }
  QByteArray Salt = Verifier.left(SaltLength);
  QByteArray Dummy = Verifier.mid(SaltLength, SaltLength);
  QByteArray Mac = Verifier.mid(SaltLength + SaltLength, MacLength);
  QByteArray Encrypted;
  QByteArray Mac2;
  AES256EncryptWithMAC(Dummy, Input, Salt, Encrypted, Mac2);
  return SameMac(Mac, Mac2);
}

static const unsigned char SScrambleTable[256] =
{
    0, 223, 235, 233, 240, 185,  88, 102,  22, 130,  27,  53,  79, 125,  66, 201,
   90,  71,  51,  60, 134, 104, 172, 244, 139,  84,  91,  12, 123, 155, 237, 151,
  192,   6,  87,  32, 211,  38, 149,  75, 164, 145,  52, 200, 224, 226, 156,  50,
  136, 190, 232,  63, 129, 209, 181, 120,  28,  99, 168,  94, 198,  40, 238, 112,
   55, 217, 124,  62, 227,  30,  36, 242, 208, 138, 174, 231,  26,  54, 214, 148,
   37, 157,  19, 137, 187, 111, 228,  39, 110,  17, 197, 229, 118, 246, 153,  80,
   21, 128,  69, 117, 234,  35,  58,  67,  92,   7, 132, 189,   5, 103,  10,  15,
  252, 195,  70, 147, 241, 202, 107,  49,  20, 251, 133,  76, 204,  73, 203, 135,
  184,  78, 194, 183,   1, 121, 109,  11, 143, 144, 171, 161,  48, 205, 245,  46,
   31,  72, 169, 131, 239, 160,  25, 207, 218, 146,  43, 140, 127, 255,  81,  98,
   42, 115, 173, 142, 114,  13,   2, 219,  57,  56,  24, 126,   3, 230,  47, 215,
    9,  44, 159,  33, 249,  18,  93,  95,  29, 113, 220,  89,  97, 182, 248,  64,
   68,  34,   4,  82,  74, 196, 213, 165, 179, 250, 108, 254,  59,  14, 236, 175,
   85, 199,  83, 106,  77, 178, 167, 225,  45, 247, 163, 158,   8, 221,  61, 191,
  119,  16, 253, 105, 186,  23, 170, 100, 216,  65, 162, 122, 150, 176, 154, 193,
  206, 222, 188, 152, 210, 243,  96,  41,  86, 180, 101, 177, 166, 141, 212, 116
};

static const unsigned char * UnscrambleTable()
{
  static unsigned char Table[256];
  static bool Initialized = []()
  {
    for (int Index = 0; Index < 256; Index++)
    {
      Table[SScrambleTable[Index]] = static_cast<unsigned char>(Index);
    }
    return true;
  }();
  Q_UNUSED(Initialized);
  return Table;
}

QByteArray ScramblePassword(const QString & Password)
{
  QByteArray UtfPassword = Password.toUtf8();
  int Len = UtfPassword.size();
  int Padding = (((Len + 3) / 17) * 17 + 17) - 3 - Len;
  QByteArray Buf;
  Buf.reserve(Padding + 3 + Len);
  auto Random = QRandomGenerator::system();
  for (int Index = 0; Index < Padding; Index++)
  {
    // any byte but NUL and digits
    int P;
    do
    {
      P = static_cast<int>(Random->bounded(1, 256));
    }
    while ((P >= '0') && (P <= '9'));
    Buf += static_cast<char>(P);
  }
  Buf += static_cast<char>('0' + (Len % 10));
  Buf += static_cast<char>('0' + ((Len / 10) % 10));
  Buf += static_cast<char>('0' + ((Len / 100) % 10));
  Buf += UtfPassword;
  WipeBytes(UtfPassword);
  int Last = 31;
  for (int Index = 0; Index < Buf.size(); Index++)
  {
    if (Buf[Index] == '\0')
    {
      // strcpy in WinSCP stops at NUL
      Buf.truncate(Index);
      break;
    }
    Last = (Last + static_cast<unsigned char>(Buf[Index])) % 255 + 1;
    Buf[Index] = static_cast<char>(SScrambleTable[Last]);
  }
  return Buf;
}

bool UnscramblePassword(const QByteArray & AScrambled, QString & Password)
{
  QByteArray Scrambled = AScrambled;
  int End = Scrambled.indexOf('\0');
  if (End >= 0)
  {
    Scrambled.truncate(End);
  }
  const unsigned char * Unscramble = UnscrambleTable();
  int Last = 31;
  for (int Index = 0; Index < Scrambled.size(); Index++)
  {
    int X = static_cast<int>(Unscramble[static_cast<unsigned char>(Scrambled[Index])]) - 1 - (Last % 255);
    if (X <= 0)
    {
      X += 255;
    }
    Scrambled[Index] = static_cast<char>(X);
    Last = (Last + X) % 255 + 1;
  }

  int S = 0;
  while ((S < Scrambled.size()) && ((Scrambled[S] < '0') || (Scrambled[S] > '9')))
  {
    S++;
  }
  bool Result = false;
  if (Scrambled.size() - S >= 3)
  {
    int Len = (Scrambled[S] - '0') + 10 * (Scrambled[S + 1] - '0') + 100 * (Scrambled[S + 2] - '0');
    int Total = (((Len + 3) / 17) * 17 + 17);
    if ((Len >= 0) && (Total == Scrambled.size()) && (Total - S - 3 == Len))
    {
      Scrambled.remove(0, Scrambled.size() - Len);
      Result = true;
    }
  }
  Password = Result ? QString::fromUtf8(Scrambled) : QString();
  WipeBytes(Scrambled);
  return Result;
}

int PasswordMaxLength()
{
  return 128;
}

int IsValidPassword(const QString & Password)
{
  if (Password.isEmpty() || (Password.size() > PasswordMaxLength()))
  {
    return -1;
  }
  int A = 0;
  int B = 0;
  int C = 0;
  int D = 0;
  for (QChar Ch : Password)
  {
    if (Ch.isLower())
    {
      A = 1;
    }
    else if (Ch.isUpper())
    {
      B = 1;
    }
    else if (Ch.isDigit())
    {
      C = 1;
    }
    else
    {
      D = 1;
    }
  }
  return ((Password.size() >= 6) && ((A + B + C + D) >= 2)) ? 1 : 0;
}

//---------------------------------------------------------------------------
// windows/WinConfiguration.cpp: master password state
namespace
{
struct MasterPasswordState
{
  QMutex Mutex;
  bool Use = false;
  QString Verifier;
  QString PlainEncrypt;
  QString PlainDecrypt;
  MasterPasswordPromptFunc Prompt;
  int Session = 0;
  bool SessionAsked = false;
  int DontDecrypt = 0;
  int NoPrompt = 0;
  // Strongly encrypted forms of plain passwords (key + password -> stored value),
  // all encrypted with the master password CacheMaster. The whole configuration
  // is re-encrypted on every save (SessionData keeps plain passwords); like
  // TWinConfiguration::StronglyRecryptPassword, which keeps an already strongly
  // encrypted value, a password once encrypted (or decrypted) keeps its stored
  // form, so a save does not run PBKDF2 for each site again.
  QString CacheMaster;
  QHash<QString, QByteArray> Cache;
};
}

static MasterPasswordState & State()
{
  static MasterPasswordState Instance;
  return Instance;
}

static void Shred(QString & Str)
{
  // as WinSCP Shred: overwrite the characters before releasing the buffer;
  // a buffer shared with another QString cannot be overwritten without
  // destroying the other copy (and fill() would detach to a fresh copy)
  if (!Str.isEmpty() && Str.isDetached())
  {
    OPENSSL_cleanse(Str.data(), static_cast<size_t>(Str.size()) * sizeof(QChar));
  }
  Str.clear();
}

// Replaces a remembered password, shredding the previous value
static void SetShredded(QString & Target, const QString & Value)
{
  if (Target.constData() != Value.constData())
  {
    Shred(Target);
  }
  Target = Value;
}

static QString CacheKey(const QString & Password, const QString & Key)
{
  return Key + QLatin1Char('\n') + Password;
}

// S.Mutex has to be locked
static void CacheEncrypted(MasterPasswordState & S, const QString & MasterPassword, const QString & CacheKeyStr, const QByteArray & Stored)
{
  if (S.CacheMaster != MasterPassword)
  {
    S.Cache.clear();
    S.CacheMaster = MasterPassword;
  }
  S.Cache.insert(CacheKeyStr, Stored);
}

void SetMasterPasswordState(bool AUseMasterPassword, const QString & VerifierHex)
{
  MasterPasswordState & S = State();
  QMutexLocker Lock(&S.Mutex);
  if ((S.Use != AUseMasterPassword) || (S.Verifier != VerifierHex))
  {
    // another master password (e.g. set by other instance): forget the known one,
    // unless it still verifies
    bool Keep = AUseMasterPassword && !S.PlainDecrypt.isEmpty() &&
      AES256Verify(S.PlainDecrypt, HexToBytes(VerifierHex));
    S.Use = AUseMasterPassword;
    S.Verifier = VerifierHex;
    if (Keep)
    {
      SetShredded(S.PlainEncrypt, S.PlainDecrypt);
    }
    else if (S.Use)
    {
      Shred(S.PlainEncrypt);
      Shred(S.PlainDecrypt);
    }
    else
    {
      Shred(S.PlainEncrypt);
    }
  }
}

bool UseMasterPassword()
{
  MasterPasswordState & S = State();
  QMutexLocker Lock(&S.Mutex);
  return S.Use;
}

QString MasterPasswordVerifier()
{
  MasterPasswordState & S = State();
  QMutexLocker Lock(&S.Mutex);
  return S.Verifier;
}

bool ValidateMasterPassword(const QString & Password)
{
  QString Verifier = MasterPasswordVerifier();
  return !Verifier.isEmpty() && AES256Verify(Password, HexToBytes(Verifier));
}

void SetMasterPassword(const QString & Password)
{
  // we can get here even if master password is off, when we encounter stray
  // encrypted password (e.g. manually imported site); make sure we do not
  // set encrypt password to avoid starting encrypting new passwords
  bool Valid = UseMasterPassword() && ValidateMasterPassword(Password);
  MasterPasswordState & S = State();
  QMutexLocker Lock(&S.Mutex);
  if (!S.Use)
  {
    SetShredded(S.PlainDecrypt, Password);
  }
  else if (Valid)
  {
    SetShredded(S.PlainEncrypt, Password);
    SetShredded(S.PlainDecrypt, Password);
  }
}

void ChangeMasterPassword(const QString & Password)
{
  QByteArray Verifier;
  AES256CreateVerifier(Password, Verifier);
  MasterPasswordState & S = State();
  QMutexLocker Lock(&S.Mutex);
  S.Verifier = BytesToHex(Verifier);
  S.Use = true;
  SetShredded(S.PlainEncrypt, Password);
  // decrypt password is switched by Configuration::ChangeMasterPassword,
  // once the existing passwords are decrypted
}

void ClearMasterPassword()
{
  MasterPasswordState & S = State();
  QMutexLocker Lock(&S.Mutex);
  S.Verifier.clear();
  S.Use = false;
  Shred(S.PlainEncrypt);
  S.Cache.clear();
  S.CacheMaster.clear();
}

bool IsMasterPasswordKnown()
{
  MasterPasswordState & S = State();
  QMutexLocker Lock(&S.Mutex);
  return !S.PlainEncrypt.isEmpty();
}

void SetMasterPasswordPrompt(const MasterPasswordPromptFunc & Prompt)
{
  MasterPasswordState & S = State();
  QMutexLocker Lock(&S.Mutex);
  S.Prompt = Prompt;
}

// TWinConfiguration::AskForMasterPassword
static bool AskForMasterPassword()
{
  MasterPasswordState & S = State();
  MasterPasswordPromptFunc Prompt;
  {
    QMutexLocker Lock(&S.Mutex);
    if (S.NoPrompt > 0)
    {
      return false;
    }
    if (S.Session > 0)
    {
      if (S.SessionAsked)
      {
        return false;
      }
      // set before the prompt, as it may be cancelled
      S.SessionAsked = true;
    }
    Prompt = S.Prompt;
  }
  // the prompt calls SetMasterPassword, so the lock must not be held
  return Prompt && Prompt();
}

bool AskForMasterPasswordIfNotSet()
{
  return IsMasterPasswordKnown() || AskForMasterPassword();
}

MasterPasswordSession::MasterPasswordSession()
{
  MasterPasswordState & S = State();
  QMutexLocker Lock(&S.Mutex);
  S.Session++;
}

MasterPasswordSession::~MasterPasswordSession()
{
  MasterPasswordState & S = State();
  QMutexLocker Lock(&S.Mutex);
  if (S.Session > 0)
  {
    S.Session--;
  }
  S.SessionAsked = false;
}

NoMasterPasswordPrompt::NoMasterPasswordPrompt()
{
  MasterPasswordState & S = State();
  QMutexLocker Lock(&S.Mutex);
  S.NoPrompt++;
}

NoMasterPasswordPrompt::~NoMasterPasswordPrompt()
{
  MasterPasswordState & S = State();
  QMutexLocker Lock(&S.Mutex);
  S.NoPrompt--;
}

DontDecryptPasswords::DontDecryptPasswords()
{
  MasterPasswordState & S = State();
  QMutexLocker Lock(&S.Mutex);
  S.DontDecrypt++;
}

DontDecryptPasswords::~DontDecryptPasswords()
{
  MasterPasswordState & S = State();
  QMutexLocker Lock(&S.Mutex);
  S.DontDecrypt--;
}

//---------------------------------------------------------------------------
QByteArray EncryptPassword(const QString & Password, const QString & Key)
{
  // TWinConfiguration::StronglyRecryptPassword
  MasterPasswordState & S = State();
  QString MasterPassword;
  bool Use;
  {
    QMutexLocker Lock(&S.Mutex);
    Use = S.Use;
    MasterPassword = S.PlainEncrypt;
  }
  if (Use && !Password.isEmpty())
  {
    if (MasterPassword.isEmpty() && AskForMasterPassword())
    {
      QMutexLocker Lock(&S.Mutex);
      MasterPassword = S.PlainEncrypt;
    }
    if (!MasterPassword.isEmpty())
    {
      QString CacheKeyStr = CacheKey(Password, Key);
      {
        QMutexLocker Lock(&S.Mutex);
        if (S.CacheMaster == MasterPassword)
        {
          auto It = S.Cache.constFind(CacheKeyStr);
          if (It != S.Cache.constEnd())
          {
            return It.value();
          }
        }
      }
      QByteArray Result;
      QByteArray Scrambled = ScramblePassword(Password);
      AES256EncryptWithMAC(Scrambled, MasterPassword, Result);
      WipeBytes(Scrambled);
      Result = SetExternalEncryptedPassword(Result);
      QMutexLocker Lock(&S.Mutex);
      CacheEncrypted(S, MasterPassword, CacheKeyStr, Result);
      return Result;
    }
    // master password not entered: the password can be stored obfuscated only
  }
  return SimpleEncryptPassword(Password, Key);
}

// Decrypts a strongly encrypted password with the known master password;
// Prompt = ask for the master password, when not known
static bool DecryptExternal(const QByteArray & Buf, bool Prompt, QString & Result, QString * UsedMasterPassword = nullptr)
{
  MasterPasswordState & S = State();
  QString MasterPassword;
  int DontDecrypt;
  {
    QMutexLocker Lock(&S.Mutex);
    MasterPassword = S.PlainDecrypt;
    DontDecrypt = S.DontDecrypt;
  }
  if (DontDecrypt > 0)
  {
    return false;
  }
  if (MasterPassword.isEmpty())
  {
    if (!Prompt || !AskForMasterPassword())
    {
      return false;
    }
    QMutexLocker Lock(&S.Mutex);
    MasterPassword = S.PlainDecrypt;
  }
  QByteArray Decrypted;
  bool Ok =
    AES256DecryptWithMAC(Buf, MasterPassword, Decrypted) &&
    UnscramblePassword(Decrypted, Result);
  WipeBytes(Decrypted);
  if (Ok && (UsedMasterPassword != nullptr))
  {
    *UsedMasterPassword = MasterPassword;
  }
  return Ok;
}

QString DecryptPassword(const QByteArray & Password, const QString & Key)
{
  QByteArray Buf;
  if (GetExternalEncryptedPassword(Password, Buf))
  {
    QString Result;
    QString MasterPassword;
    if (!DecryptExternal(Buf, true, Result, &MasterPassword))
    {
      // DECRYPT_PASSWORD_ERROR (WinSCP throws; the stored value is kept untouched)
      Result.clear();
    }
    else if (!Result.isEmpty())
    {
      // saving the password again keeps the stored form
      MasterPasswordState & S = State();
      QMutexLocker Lock(&S.Mutex);
      CacheEncrypted(S, MasterPassword, CacheKey(Result, Key), Password);
    }
    return Result;
  }
  return SimpleDecryptPassword(Password, Key);
}

bool SamePassword(const QByteArray & Password1, const QByteArray & Password2)
{
  if (Password1 == Password2)
  {
    return true;
  }
  QByteArray Buf1;
  QByteArray Buf2;
  bool External1 = GetExternalEncryptedPassword(Password1, Buf1);
  bool External2 = GetExternalEncryptedPassword(Password2, Buf2);
  if (External1 != External2)
  {
    return false;
  }
  if (External1)
  {
    QString Plain1;
    QString Plain2;
    return
      DecryptExternal(Buf1, false, Plain1) && DecryptExternal(Buf2, false, Plain2) &&
      (Plain1 == Plain2);
  }
  // compare including the key (user name + host name) prefix
  return SimpleDecrypt(Password1, QByteArray(), false) == SimpleDecrypt(Password2, QByteArray(), false);
}

}
