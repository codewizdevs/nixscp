// NixSCP Pro cloud sync tests (core/CloudCrypto, core/CloudSettings,
// CloudSync::Decide): snapshot encryption and authentication, recovery keys,
// settings normalization and the sync decision of nixscp-cloud docs/SYNC.md
#include "core/CloudCrypto.h"
#include "core/CloudSettings.h"
#include "core/CloudSync.h"
#include "core/Configuration.h"
#include "core/IniStorage.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <cstdio>

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
  // The sync state file keeps it for the next start, readable by the owner only
  QFileInfo StateFile(Dir + QStringLiteral("/nixscp-cloud.ini"));
  CHECK(StateFile.exists());
  CHECK(StateFile.permissions() & QFileDevice::ReadOwner);
#ifndef _WIN32
  // (on Windows the per-user profile ACL protects it)
  CHECK(!(StateFile.permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther)));
#endif
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
  TestRecoveryKeyText();
  TestNormalization();
  TestDecide();
  if (Failures == 0)
  {
    std::printf("All cloud sync tests passed\n");
  }
  return (Failures == 0) ? 0 : 1;
}
