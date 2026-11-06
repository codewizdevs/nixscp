// NixSCP Pro cloud sync tests (core/CloudCrypto, core/CloudSettings,
// CloudSync::Decide): snapshot encryption and authentication, recovery keys,
// settings normalization and the sync decision of nixscp-cloud docs/SYNC.md
#include "core/CloudClient.h"
#include "core/CloudCrypto.h"
#include "core/CloudSettings.h"
#include "core/CloudSync.h"
#include "core/ConfigVault.h"
#include "core/Configuration.h"
#include "core/IniStorage.h"
#include "core/Security.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtEndian>
#include <cstdio>

#ifdef Q_OS_LINUX
#include <sys/resource.h>
#endif

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); Failures++; } } while (0)

using namespace CloudCrypto;

// Small Argon2 parameters keep the tests fast; the defaults are tested once
static SnapshotHeader TestHeader(const QString & Slot = QStringLiteral("settings"), qint64 Version = 1)
{
  SnapshotHeader Header;
  Header.Slot = Slot;
  Header.User = QStringLiteral("11111111-2222-3333-4444-555555555555");
  Header.Version = Version;
  Header.SavedAt = QDateTime::fromString(QStringLiteral("2026-11-02T13:00:00.123Z"), Qt::ISODateWithMs);
  Header.Device = QStringLiteral("test-pc");
  Header.Kdf.MemoryKib = 256;
  Header.Kdf.Iterations = 1;
  return Header;
}

static void TestDefaultKdf()
{
  KdfParams Params;
  Params.Salt = QByteArray(16, 'x');
  QElapsedTimer Timer;
  Timer.start();
  QString Error;
  QByteArray A = DeriveKey(QStringLiteral("correct horse"), Params, &Error);
  CHECK(A.size() == KeyLength);
  CHECK(Error.isEmpty());
  std::printf("Argon2id 64 MiB / 3 passes: %lld ms\n", static_cast<long long>(Timer.elapsed()));
  // Deterministic, and NFKC-normalized ("ﬁ" ligature == "fi")
  Params.MemoryKib = 256;
  Params.Iterations = 1;
  CHECK(DeriveKey(QStringLiteral("pass"), Params) == DeriveKey(QStringLiteral("pass"), Params));
  CHECK(DeriveKey(QString::fromUtf8("\xEF\xAC\x81"), Params) == DeriveKey(QStringLiteral("fi"), Params));
  CHECK(DeriveKey(QStringLiteral("pass"), Params) != DeriveKey(QStringLiteral("Pass"), Params));
  // Hostile parameters from a header are refused
  KdfParams Huge = Params;
  Huge.MemoryKib = 64u * 1024u * 1024u;
  CHECK(DeriveKey(QStringLiteral("pass"), Huge).isEmpty());
}

static void TestSealOpen()
{
  QByteArray Key = RandomBytes(KeyLength);
  QByteArray Sealed = Seal(Key, "secret", "aad");
  QByteArray Plain;
  CHECK(Open(Key, Sealed, "aad", Plain) && (Plain == "secret"));
  CHECK(!Open(Key, Sealed, "other aad", Plain));
  CHECK(!Open(RandomBytes(KeyLength), Sealed, "aad", Plain));
  QByteArray Tampered = Sealed;
  Tampered[15] = static_cast<char>(Tampered[15] ^ 1);
  CHECK(!Open(Key, Tampered, "aad", Plain));
  // Random nonce: the same plaintext seals differently
  CHECK(Seal(Key, "secret", "aad") != Sealed);
}

static void TestWrapAndSnapshot()
{
  QByteArray Dek = RandomBytes(KeyLength);
  QByteArray Recovery = NewRecoveryKey();
  SnapshotHeader Header = TestHeader();
  CHECK(WrapDek(Header, Dek, QStringLiteral("sync password"), Recovery));
  CHECK(!Header.DekRecovery.isEmpty());

  QByteArray Ini = "[Sessions\\Server]\nHostName=example.com\nUserName=me\n";
  QByteArray Snapshot = EncodeSnapshot(Header, Dek, Ini);
  CHECK(Snapshot.startsWith("NXS1"));
  CHECK(!Snapshot.contains("example.com"));

  // Another computer: only the header and the password
  SnapshotHeader Read;
  CHECK(ReadHeader(Snapshot, Read));
  CHECK(Read.Slot == QStringLiteral("settings"));
  CHECK(Read.Version == 1);
  CHECK(Read.SavedAt == Header.SavedAt);
  CHECK(Read.Device == QStringLiteral("test-pc"));
  CHECK(UnwrapDekWithPassword(Read, QStringLiteral("wrong password")).isEmpty());
  QByteArray Unwrapped = UnwrapDekWithPassword(Read, QStringLiteral("sync password"));
  CHECK(Unwrapped == Dek);
  CHECK(UnwrapDekWithRecoveryKey(Read, Recovery) == Dek);
  CHECK(UnwrapDekWithRecoveryKey(Read, NewRecoveryKey()).isEmpty());

  SnapshotHeader Decoded;
  QByteArray Plain;
  QString Error;
  CHECK(DecodeSnapshot(Snapshot, Unwrapped, Decoded, Plain, &Error));
  CHECK(Plain == Ini);
  CHECK(!DecodeSnapshot(Snapshot, RandomBytes(KeyLength), Decoded, Plain, &Error));

  // The header is authenticated: a server changing version/slot/user is detected
  for (const QByteArray & From : {QByteArray("\"version\":1"), QByteArray("\"slot\":\"settings\""), QByteArray("\"user\":\"1111")})
  {
    QByteArray To = From;
    To.replace("1", "2").replace("settings", "sessionx");
    QByteArray Forged = Snapshot;
    int Index = static_cast<int>(Forged.indexOf(From));
    CHECK(Index > 0);
    if (To.size() == From.size())
    {
      Forged.replace(Index, From.size(), To);
      CHECK(!DecodeSnapshot(Forged, Dek, Decoded, Plain, &Error));
    }
  }
  // The DEK wrap is bound to the account
  SnapshotHeader OtherUser = Read;
  OtherUser.User = QStringLiteral("99999999-2222-3333-4444-555555555555");
  CHECK(UnwrapDekWithPassword(OtherUser, QStringLiteral("sync password")).isEmpty());

  // Truncated or foreign data
  CHECK(!ReadHeader("garbage", Read));
  CHECK(!DecodeSnapshot(Snapshot.left(Snapshot.size() - 1), Dek, Decoded, Plain, &Error));
  // Empty plaintext (no open tabs) round-trips
  QByteArray Empty = EncodeSnapshot(TestHeader(QStringLiteral("session")), Dek, QByteArray());
  CHECK(DecodeSnapshot(Empty, Dek, Decoded, Plain, &Error) && Plain.isEmpty());
}

// A snapshot whose packed payload is exactly Packed, as a key holder can make it
static QByteArray WithPayload(const QByteArray & Dek, const QByteArray & Packed)
{
  QByteArray Snapshot = EncodeSnapshot(TestHeader(), Dek, QByteArray());
  quint32 Length = qFromLittleEndian<quint32>(Snapshot.constData() + 4);
  QByteArray Prefix = Snapshot.left(8 + Length);
  return Prefix + Seal(Dek, Packed, Prefix);
}

static QByteArray Be32(quint32 Value)
{
  char Bytes[4];
  qToBigEndian<quint32>(Value, Bytes);
  return QByteArray(Bytes, 4);
}

static void TestBoundedPayload()
{
  QByteArray Dek = RandomBytes(KeyLength);
  SnapshotHeader Decoded;
  QByteArray Plain;
  QString Error;
  // More than one block, compressible and not, keeps its bytes
  QByteArray Large = RandomBytes(150 * 1024) + QByteArray(300 * 1024, 'x');
  CHECK(DecodeSnapshot(EncodeSnapshot(TestHeader(), Dek, Large), Dek, Decoded, Plain, &Error) && (Plain == Large));
  // Compresses: the blocks do not lose what compression is for
  CHECK(EncodeSnapshot(TestHeader(), Dek, QByteArray(1024 * 1024, 'x')).size() < 64 * 1024);

  // A plaintext length above the limit is refused before anything is inflated
  CHECK(!DecodeSnapshot(WithPayload(Dek, Be32(0xFFFFFFFFu)), Dek, Decoded, Plain, &Error));
  CHECK(!DecodeSnapshot(WithPayload(Dek, Be32(MaxPlaintextLength + 1)), Dek, Decoded, Plain, &Error));
  // A bomb: zeros that inflate far beyond the length it states
  QByteArray Bomb = qCompress(QByteArray(32 * 1024 * 1024, '\0'), 9);
  QByteArray Lying = Bomb;
  Lying.replace(0, 4, Be32(100));
  CHECK(Bomb.size() < 64 * 1024);
  CHECK(!DecodeSnapshot(WithPayload(Dek, Be32(100) + Be32(Lying.size()) + Lying), Dek, Decoded, Plain, &Error));
  CHECK(Plain.isEmpty());
  // The same stating its real length: above what one block may hold
  CHECK(!DecodeSnapshot(WithPayload(Dek, Be32(MaxPlaintextLength) + Be32(Bomb.size()) + Bomb), Dek, Decoded, Plain, &Error));
  // A length hint of 4 GiB would be allocated by qUncompress as it stands
  QByteArray Huge = qCompress(QByteArray("x"), 9);
  Huge.replace(0, 4, Be32(0xFFFFFFF0u));
  CHECK(!DecodeSnapshot(WithPayload(Dek, Be32(1) + Be32(Huge.size()) + Huge), Dek, Decoded, Plain, &Error));
  // Blocks adding up to more, or less, than the stated length; a cut block
  QByteArray Block = qCompress(QByteArray(100, 'a'), 9);
  QByteArray One = Be32(Block.size()) + Block;
  CHECK(DecodeSnapshot(WithPayload(Dek, Be32(100) + One), Dek, Decoded, Plain, &Error) && (Plain == QByteArray(100, 'a')));
  CHECK(!DecodeSnapshot(WithPayload(Dek, Be32(100) + One + One), Dek, Decoded, Plain, &Error));
  CHECK(!DecodeSnapshot(WithPayload(Dek, Be32(200) + One), Dek, Decoded, Plain, &Error));
  CHECK(!DecodeSnapshot(WithPayload(Dek, Be32(100) + One.left(One.size() - 1)), Dek, Decoded, Plain, &Error));
  CHECK(!DecodeSnapshot(WithPayload(Dek, Be32(100) + Be32(0xFFFFFFFFu) + Block), Dek, Decoded, Plain, &Error));
  CHECK(!DecodeSnapshot(WithPayload(Dek, QByteArray("x")), Dek, Decoded, Plain, &Error));

  // Blocks that are all honest, and only add up: 100 bytes each for 64 KiB of
  // zeros, so 5 MiB of them would be gigabytes. The stated total stops them.
  const int FullBlock = 64 * 1024;
  QByteArray Zeros = qCompress(QByteArray(FullBlock, '\0'), 9);
  QByteArray OneZeroBlock = Be32(Zeros.size()) + Zeros;
  int AllowedBlocks = MaxPlaintextLength / FullBlock;
  CHECK(DecodeSnapshot(WithPayload(Dek, Be32(MaxPlaintextLength) + OneZeroBlock.repeated(AllowedBlocks)), Dek, Decoded, Plain, &Error));
  CHECK(Plain.size() == MaxPlaintextLength);
  CHECK(!DecodeSnapshot(WithPayload(Dek, Be32(MaxPlaintextLength + FullBlock) + OneZeroBlock.repeated(AllowedBlocks + 1)), Dek, Decoded, Plain, &Error));
  CHECK(Plain.isEmpty());

#ifdef Q_OS_LINUX
  // Lying blocks in a row: the first one ends it, so the memory of one block is
  // all this costs, not of all forty (1.3 GiB)
  struct rusage Before, After;
  getrusage(RUSAGE_SELF, &Before);
  CHECK(!DecodeSnapshot(WithPayload(Dek, Be32(100) + (Be32(Lying.size()) + Lying).repeated(40)), Dek, Decoded, Plain, &Error));
  getrusage(RUSAGE_SELF, &After);
  CHECK(After.ru_maxrss - Before.ru_maxrss < 300 * 1024);
#endif
}

static void TestEpochsAndRotation()
{
  QByteArray OldDek = RandomBytes(KeyLength);
  QByteArray NewDek = RandomBytes(KeyLength);
  SnapshotHeader Header = TestHeader();
  Header.KeyEpoch = 2;
  Header.WrapEpoch = 5;
  Header.PrevVersion = 41;
  Header.PrevHash = QStringLiteral("ab12");
  Header.Rotated = RotationProof(OldDek, Header.User, Header.KeyEpoch);
  CHECK(WrapDek(Header, NewDek, QStringLiteral("sync password"), QByteArray()));
  QByteArray Snapshot = EncodeSnapshot(Header, NewDek, "data");
  SnapshotHeader Read;
  CHECK(ReadHeader(Snapshot, Read));
  CHECK((Read.KeyEpoch == 2) && (Read.WrapEpoch == 5) && (Read.PrevVersion == 41) && (Read.PrevHash == QStringLiteral("ab12")));
  // The old key cannot read the snapshot, but learns that it was replaced
  QByteArray Plain;
  CHECK(!DecodeSnapshot(Snapshot, OldDek, Read, Plain));
  CHECK(ProvesRotation(Read, OldDek));
  // A stranger's snapshot proves nothing, nor does a proof moved elsewhere
  CHECK(!ProvesRotation(Read, RandomBytes(KeyLength)));
  SnapshotHeader Moved = Read;
  Moved.KeyEpoch = 3;
  CHECK(!ProvesRotation(Moved, OldDek));
  Moved = Read;
  Moved.User = QStringLiteral("99999999-2222-3333-4444-555555555555");
  CHECK(!ProvesRotation(Moved, OldDek));
  Moved = Read;
  Moved.Rotated.clear();
  CHECK(!ProvesRotation(Moved, OldDek));

  // The new fields are covered by the tag, as the old ones
  for (const QByteArray & From : {QByteArray("\"key_epoch\":2"), QByteArray("\"wrap_epoch\":5"), QByteArray("\"prev_version\":41"), QByteArray("\"ab12\"")})
  {
    QByteArray Forged = Snapshot;
    qsizetype Index = Forged.indexOf(From);
    CHECK(Index > 0);
    Forged[Index + From.size() - 2] = static_cast<char>(Forged.at(Index + From.size() - 2) ^ 1);
    CHECK(!DecodeSnapshot(Forged, NewDek, Read, Plain));
  }
  // Snapshots of another format are refused
  QByteArray Other = Snapshot;
  Other.replace("\"fmt\":2", "\"fmt\":1");
  CHECK(!ReadHeader(Other, Read));
  // ...and an older one is known for what it is, not taken for damage
  CHECK(IsOldFormat(Other));
  CHECK(!IsOldFormat(Snapshot));
  CHECK(!IsOldFormat("NXS1 not a snapshot"));
  CHECK(!IsOldFormat(QByteArray(Snapshot).replace("\"fmt\":2", "\"fmt\":3")));

  // user_keys moves to the new key, with the identity key's signature of the
  // change (RFC 8032 test vector 1 proves the signature itself)
  QByteArray Seed = QByteArray::fromHex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
  CHECK(Ed25519Public(Seed).toHex() == "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
  CHECK(Ed25519Sign(Seed, QByteArray()).toHex() == "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
  for (bool IdentityFirst : {false, true})
  {
    QByteArray Box = RandomBytes(32);
    QByteArray Private = IdentityFirst ? Seed + Box : Box + Seed;
    QJsonObject Keys
    {
      {QStringLiteral("public_key"), QStringLiteral("cHVibGlj")},
      {QStringLiteral("identity_key"), QString::fromLatin1(Ed25519Public(Seed).toBase64())},
      {QStringLiteral("key_signature"), QStringLiteral("c2ln")},
      {QStringLiteral("wrapped_private_key"), QString::fromLatin1(Seal(OldDek, Private, QByteArray()).toBase64())},
    };
    QJsonObject Request = CloudSync::RewrapUserKeys(Header.User, Keys, OldDek, NewDek);
    QByteArray Wrapped = QByteArray::fromBase64(Request.value(QStringLiteral("wrapped_private_key")).toString().toLatin1());
    CHECK(Open(NewDek, Wrapped, QByteArray(), Plain) && (Plain == Private));
    CHECK(!Open(OldDek, Wrapped, QByteArray(), Plain));
    CHECK(Request.value(QStringLiteral("public_key")) == Keys.value(QStringLiteral("public_key")));
    CHECK(Request.value(QStringLiteral("identity_key")) == Keys.value(QStringLiteral("identity_key")));
    CHECK(Request.value(QStringLiteral("key_signature")) == Keys.value(QStringLiteral("key_signature")));
    QByteArray Signed = QStringLiteral("cHVibGlj|%1|%2").arg(Keys.value(QStringLiteral("identity_key")).toString(),
      Request.value(QStringLiteral("wrapped_private_key")).toString()).toUtf8();
    QByteArray Message = "NXT2-KEYS-REPLACE|" + Header.User.toUtf8() + '|' + CloudSettings::Hash(Signed).toLatin1();
    CHECK(QByteArray::fromBase64(Request.value(QStringLiteral("replace_signature")).toString().toLatin1()) == Ed25519Sign(Seed, Message));
    // Not with another key, and not without the identity key in the blob
    CHECK(CloudSync::RewrapUserKeys(Header.User, Keys, RandomBytes(KeyLength), NewDek).isEmpty());
    QJsonObject Other = Keys;
    Other.insert(QStringLiteral("identity_key"), QString::fromLatin1(Ed25519Public(RandomBytes(32)).toBase64()));
    CHECK(CloudSync::RewrapUserKeys(Header.User, Other, OldDek, NewDek).isEmpty());
  }

  // The recovery wrap alone can be replaced
  QByteArray Recovery = NewRecoveryKey();
  SnapshotHeader Wrap = TestHeader();
  CHECK(WrapDek(Wrap, NewDek, QStringLiteral("sync password"), Recovery));
  QByteArray PasswordWrap = Wrap.DekPassword;
  QByteArray Replacement = NewRecoveryKey();
  WrapDekForRecovery(Wrap, NewDek, Replacement);
  CHECK(Wrap.DekPassword == PasswordWrap);
  CHECK(UnwrapDekWithRecoveryKey(Wrap, Replacement) == NewDek);
  CHECK(UnwrapDekWithRecoveryKey(Wrap, Recovery).isEmpty());
}

static void TestPasswordStrength()
{
  // Weak: too short, common, repeated, runs, or the same in disguise
  for (const char * Weak : {"short", "password", "aaaaaaaaaaaaaaaa", "abcdefghijklmnop", "1234567890123", "qwertyuiop12",
         "Password1234", "P@ssw0rd12345", "passwordpassword", "letmein123456", "qwertyqwertyqwerty", "0000000000000000", "Summer2026!!"})
  {
    CHECK(!PasswordProblem(QString::fromLatin1(Weak)).isEmpty());
    CHECK(PasswordScore(QString::fromLatin1(Weak)) == 0);
  }
  // Long enough to be worth an attacker's time, also of plain words
  for (const char * Good : {"correct horse battery staple", "live test sync password", "agent test NixSCP password", "xk4-Tq9!mZp2vR"})
  {
    CHECK(PasswordProblem(QString::fromLatin1(Good)).isEmpty());
    CHECK(PasswordScore(QString::fromLatin1(Good)) >= 1);
  }
  CHECK(PasswordScore(QStringLiteral("correct horse battery staple")) > PasswordScore(QStringLiteral("xk4tq9mzp2vr")));
  // The account's address is the first thing to try
  CHECK(!PasswordProblem(QStringLiteral("dragan.plavsic-xk4tq9mzp"), QStringLiteral("dragan.plavsic@example.com")).isEmpty());
  CHECK(PasswordProblem(QStringLiteral("dragan.plavsic-xk4tq9mzp"), QStringLiteral("someone@example.com")).isEmpty());
  // Generated: long, different each time, and accepted
  QString One = NewPassphrase();
  CHECK(One.size() == 35);
  CHECK(One != NewPassphrase());
  CHECK(PasswordProblem(One).isEmpty());
  CHECK(PasswordScore(One) == 3);
}

static void TestBaseUrl()
{
  using C = CloudClient;
  CHECK(C::AcceptableUrl(QStringLiteral("https://nixscp.com"), false));
  CHECK(C::AcceptableUrl(QStringLiteral("https://cloud.example.com:8443"), false));
  // Plain http: never from a file
  CHECK(!C::AcceptableUrl(QStringLiteral("http://nixscp.com"), false));
  CHECK(!C::AcceptableUrl(QStringLiteral("http://127.0.0.1:3222"), false));
  // From the environment for a server on this computer
  CHECK(C::AcceptableUrl(QStringLiteral("http://127.0.0.1:3222"), true));
  CHECK(C::AcceptableUrl(QStringLiteral("http://localhost:3222"), true));
  CHECK(C::AcceptableUrl(QStringLiteral("http://[::1]:3222"), true));
#ifdef QT_NO_DEBUG
  CHECK(!C::AcceptableUrl(QStringLiteral("http://evil.example"), true));
  CHECK(!C::AcceptableUrl(QStringLiteral("http://127.0.0.1.evil.example"), true));
#endif
  for (const char * Bad : {"", "nixscp.com", "file:///etc/passwd", "ftp://nixscp.com", "https://"})
  {
    CHECK(!C::AcceptableUrl(QString::fromLatin1(Bad), true));
  }
  // What is not acceptable falls back to the default, with or without the variable
  qunsetenv("NIXSCP_CLOUD_URL");
  CHECK(C::DefaultBaseUrl() == QStringLiteral("https://nixscp.com"));
  qputenv("NIXSCP_CLOUD_URL", "http://evil.example/");
#ifdef QT_NO_DEBUG
  CHECK(C::DefaultBaseUrl() == QStringLiteral("https://nixscp.com"));
#endif
  qputenv("NIXSCP_CLOUD_URL", "http://127.0.0.1:39999/");
  CHECK(C::DefaultBaseUrl() == QStringLiteral("http://127.0.0.1:39999"));
  qunsetenv("NIXSCP_CLOUD_URL");
  CloudClient Client;
  Client.SetBaseUrl(QStringLiteral("http://evil.example"));
  CHECK(Client.BaseUrl() == QStringLiteral("https://nixscp.com"));
  Client.SetBaseUrl(QStringLiteral("https://cloud.example.com/"));
  CHECK(Client.BaseUrl() == QStringLiteral("https://cloud.example.com"));
  // A signed URL from the server: only https, or the plain test server itself
  auto Refused = std::make_shared<QString>();
  Client.Transfer("GET", QUrl(QStringLiteral("http://evil.example/x")), QByteArray(), {},
    [Refused](const CloudClient::Reply & Reply) { *Refused = Reply.Error; });
  CHECK(Refused->contains(QStringLiteral("not secure")));
  Refused->clear();
  Client.Transfer("GET", QUrl(QStringLiteral("file:///etc/passwd")), QByteArray(), {},
    [Refused](const CloudClient::Reply & Reply) { *Refused = Reply.Error; });
  CHECK(Refused->contains(QStringLiteral("not secure")));
}

static void TestSecureBytes()
{
  QByteArray Key = RandomBytes(KeyLength);
  SecureBytes Secret(Key);
  CHECK((Secret.Size() == KeyLength) && !Secret.IsEmpty());
  CHECK(Secret.Equals(Key) && (Secret.View() == Key) && (Secret.Copy() == Key));
  CHECK(!Secret.Equals(RandomBytes(KeyLength)));
  CHECK(!Secret.Equals(Key.left(10)));
  // Its own bytes, not shared with what it was made from
  CHECK(Secret.View().constData() != Key.constData());
  // Copies are independent; clearing one leaves the other
  SecureBytes Other = Secret;
  CHECK(Other.View().constData() != Secret.View().constData());
  Other.Clear();
  CHECK(Other.IsEmpty() && Other.View().isEmpty() && Secret.Equals(Key));
  // Assigning a view of itself keeps the value
  Secret.Assign(Secret.View());
  CHECK(Secret.Equals(Key));
  SecureBytes Moved = std::move(Secret);
  CHECK(Moved.Equals(Key) && Secret.IsEmpty());
  // A key that is shared: the wipe must not touch the other holder's bytes
  QByteArray Shared = Key;
  QByteArray Holder = Shared;
  Security::WipeBytes(Shared);
  CHECK(Shared.isEmpty() && (Holder == Key));
}

static void TestWeakPasswordRefused(CloudSync & Sync)
{
  // Whatever the interface does: the engine refuses it too
  auto Result = std::make_shared<QString>();
  auto Ok = std::make_shared<bool>(true);
  Sync.CreateKey(QStringLiteral("password"), false, [Result, Ok](bool Done, const QString & Text) { *Ok = Done; *Result = Text; });
  CHECK(!*Ok && !Result->isEmpty());
}

static void TestRecoveryKeyText()
{
  QByteArray Key = NewRecoveryKey();
  QString Text = FormatRecoveryKey(Key);
  CHECK(Text.size() == 52 + 12);
  CHECK(Text.count(QLatin1Char('-')) == 12);
  CHECK(ParseRecoveryKey(Text) == Key);
  // Case, spaces, missing dashes and the usual look-alikes are tolerated
  QString Sloppy = Text.toLower();
  Sloppy.remove(QLatin1Char('-'));
  Sloppy.replace(QLatin1Char('0'), QLatin1Char('o'));
  Sloppy.replace(QLatin1Char('1'), QLatin1Char('l'));
  CHECK(ParseRecoveryKey(QStringLiteral("  ") + Sloppy + QStringLiteral(" ")) == Key);
  CHECK(ParseRecoveryKey(Text.left(Text.size() - 1)).isEmpty());
  CHECK(ParseRecoveryKey(Text + QStringLiteral("A")).isEmpty());
  CHECK(ParseRecoveryKey(QStringLiteral("not a key")).isEmpty());
}

static IniFile Load(const QByteArray & Text)
{
  IniFile File;
  File.LoadFromData(Text);
  return File;
}

static void TestNormalization()
{
  const QString Prefix = CloudSettings::AutoWorkspacePrefix(true, QStringLiteral("My Workspace"));
  CHECK(Prefix == QStringLiteral("Sessions\\My%20Workspace/"));
  CHECK(CloudSettings::AutoWorkspacePrefix(false, QStringLiteral("My Workspace")).isEmpty());

  IniFile Pc1 = Load(
    "[Configuration\\Interface]\nConfirmDeleting=0\nLastStoredSession=Work/A\nAutoWorkspace=My%20Workspace\n"
    "[Configuration\\Interface\\NixSCP]\nWindowGeometry=01ab\nPanelSplitter=ff\nLastLocalDirectory=/home/a\n"
    "[Configuration\\History\\LocalTarget]\n0=/home/a/x\n"
    "[Configuration\\CDCache]\nme@a.example.com=0123abcd\n"
    "[Configuration\\Interface\\Commander\\LocalPanel]\nDirViewParams=1;2\n"
    "[Sessions\\Work/A]\nHostName=a.example.com\n"
    "[Sessions\\My%20Workspace/0000]\nIsWorkspace=1\nLink=Work/A\n");
  // The same settings on another computer, in another order, other window layout
  IniFile Pc2 = Load(
    "[Sessions\\Work/A]\nHostName=a.example.com\n"
    "[Configuration\\Interface\\NixSCP]\nWindowGeometry=99cc\nLastLocalDirectory=C:/x\n"
    "[Configuration\\Interface]\nAutoWorkspace=My%20Workspace\nConfirmDeleting=0\n"
    "[Sessions\\My%20Workspace/0000]\nIsWorkspace=1\nLink=Work/B\n");
  QByteArray C1 = CloudSettings::Canonical(CloudSettings::Syncable(Pc1, Prefix));
  QByteArray C2 = CloudSettings::Canonical(CloudSettings::Syncable(Pc2, Prefix));
  CHECK(C1 == C2);
  CHECK(!C1.contains("WindowGeometry"));
  CHECK(!C1.contains("LastLocalDirectory"));
  CHECK(!C1.contains("History"));
  CHECK(!C1.contains("CDCache"));
  CHECK(!C1.contains("LocalPanel"));
  CHECK(!C1.contains("My%20Workspace/0000"));
  CHECK(C1.contains("[Sessions\\Work/A]\nHostName=a.example.com\n"));
  CHECK(CloudSettings::Hash(C1).size() == 64);
  // Without auto-save the workspace is an ordinary (synced) workspace
  CHECK(CloudSettings::Canonical(CloudSettings::Syncable(Pc1, QString())).contains("My%20Workspace/0000"));
  // A real change is visible
  IniFile Changed = Pc2;
  Changed.WriteRaw(QStringLiteral("Sessions\\Work/A"), QStringLiteral("HostName"), QStringLiteral("b.example.com"));
  CHECK(CloudSettings::Canonical(CloudSettings::Syncable(Changed, Prefix)) != C1);

  // Pulling PC2's settings on PC1 keeps PC1's window, history, panel and workspace
  IniFile Remote = Load(C2 + "[Sessions\\New]\nHostName=new.example.com\n");
  IniFile Merged = CloudSettings::MergePulled(Remote, Pc1, Prefix);
  CHECK(Merged.ReadRaw(QStringLiteral("Sessions\\New"), QStringLiteral("HostName")) == QStringLiteral("new.example.com"));
  CHECK(Merged.ReadRaw(QStringLiteral("Configuration\\Interface\\NixSCP"), QStringLiteral("WindowGeometry")) == QStringLiteral("01ab"));
  CHECK(Merged.ReadRaw(QStringLiteral("Configuration\\Interface\\NixSCP"), QStringLiteral("LastLocalDirectory")) == QStringLiteral("/home/a"));
  CHECK(Merged.ReadRaw(QStringLiteral("Configuration\\Interface"), QStringLiteral("LastStoredSession")) == QStringLiteral("Work/A"));
  CHECK(Merged.ReadRaw(QStringLiteral("Configuration\\History\\LocalTarget"), QStringLiteral("0")) == QStringLiteral("/home/a/x"));
  CHECK(Merged.ReadRaw(QStringLiteral("Configuration\\CDCache"), QStringLiteral("me@a.example.com")) == QStringLiteral("0123abcd"));
  CHECK(Merged.ReadRaw(QStringLiteral("Sessions\\My%20Workspace/0000"), QStringLiteral("Link")) == QStringLiteral("Work/A"));
  // ...and normalizes back to exactly what was pulled (no echo upload)
  CHECK(CloudSettings::Canonical(CloudSettings::Syncable(Merged, Prefix)) ==
        CloudSettings::Canonical(CloudSettings::Syncable(Remote, Prefix)));
  // The policies (root) section never travels
  IniFile WithPolicy = Load("DisablePasswordStoring=1\n[Sessions\\X]\nHostName=x\n");
  CHECK(!CloudSettings::Canonical(CloudSettings::Syncable(WithPolicy, QString())).contains("DisablePasswordStoring"));
}

static void TestDecide()
{
  using C = CloudSync;
  // First computer, empty cloud: upload
  CHECK(C::Decide(0, QString(), 0, QStringLiteral("h1")) == C::saPush);
  // Unchanged everywhere
  CHECK(C::Decide(3, QStringLiteral("h1"), 3, QStringLiteral("h1")) == C::saNone);
  // Changed here only
  CHECK(C::Decide(3, QStringLiteral("h1"), 3, QStringLiteral("h2")) == C::saPush);
  // Changed elsewhere only (the offline PC without edits fast-forwards)
  CHECK(C::Decide(3, QStringLiteral("h1"), 7, QStringLiteral("h1")) == C::saPull);
  // Offline PC with its own edits: never overwrites, the user decides
  CHECK(C::Decide(3, QStringLiteral("h1"), 7, QStringLiteral("h2")) == C::saConflict);
  // Second computer, cloud has data
  CHECK(C::Decide(0, QString(), 5, QStringLiteral("h1")) == C::saConflict);
  // The server lost versions
  CHECK(C::Decide(7, QStringLiteral("h1"), 3, QStringLiteral("h1")) == C::saRolledBack);
}

static IniFile KeyBackup(const QByteArray & Dek)
{
  IniFile File = Load("[Sessions\\Work/A]\nHostName=a.example.com\n");
  const QString & S = CloudSettings::KeySection;
  File.WriteRaw(S, QStringLiteral("Format"), QStringLiteral("1"));
  File.WriteRaw(S, QStringLiteral("User"), QStringLiteral("11111111-2222-3333-4444-555555555555"));
  File.WriteRaw(S, QStringLiteral("Email"), MungeStr(QStringLiteral("me@example.com"), true));
  File.WriteRaw(S, QStringLiteral("Dek"), QString::fromLatin1(Dek.toHex()));
  File.WriteRaw(S, QStringLiteral("MemoryKib"), QStringLiteral("256"));
  File.WriteRaw(S, QStringLiteral("Iterations"), QStringLiteral("1"));
  File.WriteRaw(S, QStringLiteral("Lanes"), QStringLiteral("1"));
  File.WriteRaw(S, QStringLiteral("Salt"), QString::fromLatin1(RandomBytes(16).toHex()));
  File.WriteRaw(S, QStringLiteral("DekPassword"), QString::fromLatin1(RandomBytes(60).toHex()));
  File.WriteRaw(S, QStringLiteral("DekRecovery"), QString());
  return File;
}

static void TestKeyBackup(const QString & Dir)
{
  Configuration & Config = Configuration::Instance();
  Config.SetIniFileName(Dir + QStringLiteral("/nixscp.ini"));
  Config.Load();
  CloudSync & Sync = CloudSync::Instance();
  Sync.Initialize(Config);
  CHECK(Sync.State() == CloudSync::csSignedOut);
  CHECK(!Sync.HasKey());
  IniFile Nothing;
  CHECK(!Sync.ExportKey(Nothing));

  // The key section never lands in the configuration, and is never synced
  QByteArray Dek = RandomBytes(KeyLength);
  CHECK(Config.ImportData(KeyBackup(Dek)));
  CHECK(!Config.ExportData().SectionExists(CloudSettings::KeySection));
  CHECK(Config.StoredSessions().FindByName(QStringLiteral("Work/A")) != nullptr);
  CHECK(!CloudSettings::Canonical(CloudSettings::Syncable(KeyBackup(Dek), QString())).contains("CloudSync"));

  // Damaged keys are refused, and taken out of the file anyway
  IniFile Damaged = KeyBackup(Dek);
  Damaged.WriteRaw(CloudSettings::KeySection, QStringLiteral("Dek"), QStringLiteral("abcd"));
  CHECK(Sync.ImportKey(Damaged) == CloudSync::kiInvalid);
  CHECK(!Damaged.SectionExists(CloudSettings::KeySection));
  CHECK(!Sync.HasKey());
  IniFile Future = KeyBackup(Dek);
  Future.WriteRaw(CloudSettings::KeySection, QStringLiteral("Format"), QStringLiteral("2"));
  CHECK(Sync.ImportKey(Future) == CloudSync::kiInvalid);
  IniFile Plain = Load("[Sessions\\X]\nHostName=x\n");
  CHECK(Sync.ImportKey(Plain) == CloudSync::kiNone);

  // Signed out: the key waits for its account
  IniFile Backup = KeyBackup(Dek);
  QString Email;
  CHECK(Sync.ImportKey(Backup, &Email) == CloudSync::kiImported);
  CHECK(Email == QStringLiteral("me@example.com"));
  CHECK(!Backup.SectionExists(CloudSettings::KeySection));
  CHECK(Backup.SectionExists(QStringLiteral("Sessions\\Work/A")));
  CHECK(Sync.HasKey());
  CHECK(Sync.State() == CloudSync::csSignedOut);

  // Export again: the same key, hex values only (nothing for INI munging)
  IniFile Exported;
  CHECK(Sync.ExportKey(Exported));
  const QString & S = CloudSettings::KeySection;
  CHECK(Exported.ReadRaw(S, QStringLiteral("Dek")) == QString::fromLatin1(Dek.toHex()));
  CHECK(Exported.ReadRaw(S, QStringLiteral("User")) == QStringLiteral("11111111-2222-3333-4444-555555555555"));
  QRegularExpression Hex(QStringLiteral("^[0-9a-f]*$"));
  for (const char * Key : {"Dek", "Salt", "DekPassword", "DekRecovery"})
  {
    CHECK(Hex.match(Exported.ReadRaw(S, QString::fromLatin1(Key))).hasMatch());
  }
  CHECK(Exported.ReadRaw(S, QStringLiteral("Email")) == MungeStr(QStringLiteral("me@example.com"), true));
  // The sync state file keeps it for the next start, sealed and readable by the owner only
  QFileInfo StateFile(Dir + QStringLiteral("/nixscp-cloud.ini"));
  CHECK(StateFile.exists());
  CHECK(StateFile.permissions() & QFileDevice::ReadOwner);
#ifndef _WIN32
  // (on Windows the per-user profile ACL protects it)
  CHECK(!(StateFile.permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther)));
#endif
  CHECK(ConfigVault::IsSealedFile(StateFile.filePath()));
  TestWeakPasswordRefused(Sync);
}

int main(int argc, char ** argv)
{
  QCoreApplication App(argc, argv);
  QStandardPaths::setTestModeEnabled(true);
  QTemporaryDir Dir;
  TestKeyBackup(Dir.path());
  TestDefaultKdf();
  TestSealOpen();
  TestWrapAndSnapshot();
  TestBoundedPayload();
  TestEpochsAndRotation();
  TestPasswordStrength();
  TestBaseUrl();
  TestSecureBytes();
  TestRecoveryKeyText();
  TestNormalization();
  TestDecide();
  if (Failures == 0)
  {
    std::printf("All cloud sync tests passed\n");
  }
  return (Failures == 0) ? 0 : 1;
}
