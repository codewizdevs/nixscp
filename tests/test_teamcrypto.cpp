// NixSCP Teams end-to-end encryption (core/TeamCrypto, core/TeamRoster,
// core/TeamContent): key pairs, identity keys and fingerprints, member wraps
// bound to team and key version, invite codes, invite wraps and verifiers,
// private keys under the sync DEK, signed team snapshots, the roster's rules and
// the allow-list of what a team shares.
#include "core/CloudCrypto.h"
#include "core/IniStorage.h"
#include "core/SessionData.h"
#include "core/TeamContent.h"
#include "core/TeamCrypto.h"
#include "core/TeamRoster.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

#include <QtEndian>

#include <cstdio>

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)

static QString NewUuid()
{
  return QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
}

// The rules of the roster, without any server
static void TestRoster()
{
  using namespace TeamRoster;
  const QString Team = NewUuid();
  const QString Owner = NewUuid();
  const QString Bob = NewUuid();
  TeamCrypto::SignKey OwnerKey = TeamCrypto::NewSignKey();
  TeamCrypto::SignKey BobKey = TeamCrypto::NewSignKey();
  TeamCrypto::KeyPair OwnerBox = TeamCrypto::NewKeyPair();
  TeamCrypto::KeyPair BobBox = TeamCrypto::NewKeyPair();
  State S;
  Entry Genesis = Make(S, Team, QStringLiteral("create"), Owner, CreateData(QStringLiteral("o@nixscp.test"), OwnerKey, Owner, OwnerBox.Public, "wrap"), OwnerKey);
  // the genesis must say who it is
  State Copy = S;
  CHECK(!Apply(Copy, Make(S, Team, QStringLiteral("invite"), Owner, QJsonObject(), OwnerKey)));
  Entry WrongSigner = Make(S, Team, QStringLiteral("create"), Owner, CreateData(QStringLiteral("o@nixscp.test"), OwnerKey, Owner, OwnerBox.Public, "wrap"), BobKey);
  CHECK(!Apply(Copy, WrongSigner));
  CHECK((Copy.Seq == 0) && Copy.Members.isEmpty());
  CHECK(Apply(S, Genesis));
  CHECK((S.Seq == 1) && (S.KeyVersion == 1) && (S.Owner == Owner) && S.CanEdit(Owner));
  // a replayed or skipped entry breaks the chain
  CHECK(!Apply(S, Genesis));
  // the box key is the identity's statement
  TeamCrypto::Invite Code = TeamCrypto::NewInvite();
  TeamCrypto::SignKey InviteKey = TeamCrypto::InviteSignKey(Code);
  QString Expires = QStringLiteral("2099-01-01T00:00:00.000Z");
  QJsonObject InviteData{{QStringLiteral("id"), Code.Id}, {QStringLiteral("role"), QStringLiteral("admin")}, {QStringLiteral("expires"), Expires},
    {QStringLiteral("public"), QString::fromLatin1(InviteKey.Public.toBase64())}, {QStringLiteral("key_version"), 1}};
  CHECK(Apply(S, Make(S, Team, QStringLiteral("invite"), Owner, InviteData, OwnerKey)));
  QJsonObject Join = JoinData(Code.Id, QStringLiteral("b@nixscp.test"), BobKey, Bob, BobBox.Public, "wrap2");
  QJsonObject BadBox = Join;
  BadBox.insert(QStringLiteral("box"), QString::fromLatin1(TeamCrypto::NewKeyPair().Public.toBase64()));
  Copy = S;
  CHECK(!Apply(Copy, Make(S, Team, QStringLiteral("join"), Bob, BadBox, BobKey, &InviteKey)));
  CHECK(Apply(S, Make(S, Team, QStringLiteral("join"), Bob, Join, BobKey, &InviteKey)));
  CHECK(S.Find(Bob)->Role == QStringLiteral("admin"));
  CHECK(S.Find(Bob)->WrapHash == WrapHash("wrap2"));
  CHECK(S.Invites.isEmpty());
  // an expired invite
  TeamCrypto::Invite Late = TeamCrypto::NewInvite();
  TeamCrypto::SignKey LateKey = TeamCrypto::InviteSignKey(Late);
  QJsonObject LateData = InviteData;
  LateData.insert(QStringLiteral("id"), Late.Id);
  LateData.insert(QStringLiteral("role"), QStringLiteral("member"));
  LateData.insert(QStringLiteral("expires"), QStringLiteral("2001-01-01T00:00:00.000Z"));
  LateData.insert(QStringLiteral("public"), QString::fromLatin1(LateKey.Public.toBase64()));
  CHECK(Apply(S, Make(S, Team, QStringLiteral("invite"), Owner, LateData, OwnerKey)));
  QString Carol = NewUuid();
  TeamCrypto::SignKey CarolKey = TeamCrypto::NewSignKey();
  Copy = S;
  CHECK(!Apply(Copy, Make(S, Team, QStringLiteral("join"), Carol, JoinData(Late.Id, QStringLiteral("c@nixscp.test"), CarolKey, Carol, BobBox.Public, "w"), CarolKey, &LateKey)));
  // an admin who made a snapshot, demoted later
  qint64 AdminSeq = S.Seq;
  CHECK(S.AuthorAllowed(Bob, AdminSeq, 5, nullptr));
  CHECK(Apply(S, Make(S, Team, QStringLiteral("role"), Owner, QJsonObject{{QStringLiteral("user"), Bob}, {QStringLiteral("role"), QStringLiteral("member")}, {QStringLiteral("head"), 5}}, OwnerKey)));
  CHECK(!S.CanEdit(Bob));
  CHECK(S.AuthorAllowed(Bob, AdminSeq, 5, nullptr));      // what they wrote before
  CHECK(!S.AuthorAllowed(Bob, AdminSeq, 6, nullptr));     // written later, claiming an old position
  CHECK(!S.AuthorAllowed(Bob, S.Seq, 6, nullptr));
  CHECK(!S.AuthorAllowed(NewUuid(), S.Seq, 6, nullptr));
  CHECK(S.AuthorAllowed(Owner, S.Seq, 6, nullptr));
  CHECK(!S.AuthorAllowed(Owner, S.Seq + 1, 6, nullptr));
  CHECK(!S.AuthorAllowed(Owner, 0, 6, nullptr));
  // rotation: exact coverage, wrap hashes signed, invites gone
  CHECK(!S.NeedsRotation);
  CHECK(Apply(S, Make(S, Team, QStringLiteral("remove"), Owner, QJsonObject{{QStringLiteral("user"), Bob}, {QStringLiteral("head"), 5}}, OwnerKey)));
  CHECK(S.NeedsRotation && S.Invites.isEmpty() && (S.Find(Bob) == nullptr));
  Copy = S;
  CHECK(!Apply(Copy, Make(S, Team, QStringLiteral("rotate"), Owner, QJsonObject{{QStringLiteral("key_version"), 2},
    {QStringLiteral("wraps"), QJsonObject{{Owner, QString(64, QLatin1Char('a'))}, {Bob, QString(64, QLatin1Char('a'))}}}}, OwnerKey)));
  CHECK(!Apply(Copy, Make(S, Team, QStringLiteral("rotate"), Owner, QJsonObject{{QStringLiteral("key_version"), 3},
    {QStringLiteral("wraps"), QJsonObject{{Owner, QString(64, QLatin1Char('a'))}}}}, OwnerKey)));
  qint64 Before = S.Seq;
  CHECK(Apply(S, Make(S, Team, QStringLiteral("rotate"), Owner, QJsonObject{{QStringLiteral("key_version"), 2},
    {QStringLiteral("wraps"), QJsonObject{{Owner, QString(64, QLatin1Char('a'))}}}}, OwnerKey)));
  CHECK((S.KeyVersion == 2) && !S.NeedsRotation);
  CHECK((S.KeyVersionAt(Before) == 1) && (S.KeyVersionAt(S.Seq) == 2) && (S.KeyVersionAt(0) == 0));
  // the owner stays
  Copy = S;
  CHECK(!Apply(Copy, Make(S, Team, QStringLiteral("leave"), Owner, QJsonObject{{QStringLiteral("head"), 0}}, OwnerKey)));
  // a whole chain from the wire; a changed entry fails it
  QJsonArray Wire;
  State Check;
  Entry One = Make(Check, Team, QStringLiteral("create"), Owner, CreateData(QStringLiteral("o@nixscp.test"), OwnerKey, Owner, OwnerBox.Public, "wrap"), OwnerKey);
  CHECK(Apply(Check, One));
  Wire.append(ToWire(One));
  State Result;
  QList<Entry> Items;
  CHECK(Replay(Wire, Team, Result, &Items) && (Items.size() == 1) && (Result.Hash == One.Hash));
  CHECK(!Replay(Wire, NewUuid(), Result));
  CHECK(!Replay(QJsonArray(), Team, Result));
  QJsonObject Changed = ToWire(One);
  QByteArray Body = One.Body;
  Body.replace("o@nixscp.test", "x@nixscp.test");
  Changed.insert(QStringLiteral("body"), QString::fromLatin1(Body.toBase64()));
  CHECK(!Replay(QJsonArray{Changed}, Team, Result));
  CHECK(!Replay(QJsonArray{QJsonValue(5)}, Team, Result));
}

// What a team may set on a member's site
static void TestContent()
{
  using namespace TeamContent;
  CHECK(NormalizeName(QStringLiteral("  Pw \t Two\n")) == QStringLiteral("Pw Two"));
  CHECK(NormalizeName(QStringLiteral("a\xE2\x80\xAE")) != QString());
  CHECK(NormalizeName(QString::fromUtf8("a\u202Eb\u0007")) == QStringLiteral("ab"));
  CHECK(NormalizeName(QString(500, QLatin1Char('x'))).size() == 100);
  CHECK(NormalizeName(QStringLiteral("   ")).isEmpty());
  CHECK(NormalizeName(QStringLiteral("<img src=x>Prod")) == QStringLiteral("img src=xProd"));
  for (const char * Key : {"HostName", "PortNumber", "UserName", "PasswordPlain", "FSProtocol", "Ftps", "RemoteDirectory", "LocalDirectory", "Note", "SshHostKey"})
  {
    CHECK(IsSharedKey(QString::fromLatin1(Key)));
  }
  // what runs a command, weakens a check, reads a local file or forwards anything
  for (const char * Key : {"ProxyMethod", "ProxyHost", "ProxyPort", "ProxyUsername", "ProxyPasswordEnc", "ProxyPassword", "ProxyTelnetCommand", "ProxyLocalCommand", "ProxyLocalhost",
         "ProxyDNS", "PostLoginCommands", "Shell", "SftpServer", "ListingCommand", "ReturnVar", "ClearAliases", "UnsetNationalVars", "AgentFwd", "TryAgent", "GSSAPIFwdTGT",
         "AuthGSSAPI", "AuthGSSAPIKEX", "AuthKI", "AuthKIPassword", "SshNoUserAuth", "ChangeUsername", "Tunnel", "TunnelHostName", "TunnelPortNumber", "TunnelUserName", "TunnelPassword",
         "TunnelPasswordPlain", "TunnelPublicKeyFile", "TunnelLocalPortNumber", "TunnelHostKey", "PortForwards", "KnownHostsFile", "LogicalHostName", "PublicKeyFile", "DetachedCertificate",
         "TlsCertificateFile", "Cipher", "KEX", "HostKey", "Ssh2DES", "MinTlsVersion", "MaxTlsVersion", "CompleteTlsShutdown", "DeleteToRecycleBin", "OverwrittenToRecycleBin", "RecycleBinPath",
         "AgentAccess", "AgentPermissions", "AgentName", "AgentAllow", "AgentDeny", "AgentRecord", "IsWorkspace", "Link", "NameOverride", "Special", "S3Profile", "S3CredentialsEnv", "S3SessionToken",
         "S3RoleArn", "S3RoleSessionName", "FtpForcePasvIp2", "FtpProxyLogonType", "SourceAddress", "Password", "BugHMAC2"})
  {
    CHECK(!IsSharedKey(QString::fromLatin1(Key)));
  }
  // allow-list, not block-list: every setting a site saves is either a key above or
  // stays local, so a new key added to SessionData::Save is not shared
  CHECK(!IsSharedKey(QStringLiteral("SomeSettingAddedNextYear")));
  // Equal sites give equal bytes, whatever the stored password's padding was
  SessionData Site;
  Site.HostName = QStringLiteral("db.internal");
  Site.UserName = QStringLiteral("deploy");
  Site.Password = QStringLiteral("S3cret");
  IniFile First;
  IniFile Second;
  CHECK(AddSite(First, QStringLiteral("sub/db"), Site));
  CHECK(AddSite(Second, QStringLiteral("sub/db"), Site));
  CHECK(Hash(First) == Hash(Second));
  Site.ProxyMethod = pmCmd;
  Site.ProxyLocalCommand = QStringLiteral("sh");
  Site.AgentFwd = true;
  CHECK(AddSite(Second, QStringLiteral("sub/db"), Site));
  CHECK(Hash(First) == Hash(Second));   // not shared, so no change
  // content
  QString Name;
  IniFile Decoded;
  QByteArray Text = Encode(QStringLiteral("My Team"), First);
  CHECK(Decode(Text, Name, Decoded) && (Name == QStringLiteral("My Team")) && (Hash(Decoded) == Hash(First)));
  CHECK(SiteHashes(Decoded).contains(QStringLiteral("sub/db")));
  CHECK(!Decode("[Sessions\\x]\nHostName=a\n", Name, Decoded));
  // names that would leave or confuse the folder
  int Dropped = 0;
  CHECK(Decode("[NixSCP\\Team]\nName=x\n[Sessions\\..%2Fup]\nHostName=a\n[Sessions\\a%2F%2Fb]\nHostName=a\n[Sessions\\%2Fabs]\nHostName=a\n"
    "[Sessions\\a\\b]\nHostName=a\n[Other]\nx=1\n", Name, Decoded, &Dropped));
  CHECK(SiteHashes(Decoded).isEmpty());
  CHECK(Dropped >= 4);
  // values
  auto Value = [&](const QByteArray & Line)
  {
    IniFile Result;
    QString TeamName;
    Decode("[NixSCP\\Team]\nName=x\n[Sessions\\s]\nHostName=ok.example\n" + Line + "\n", TeamName, Result);
    return Result.Keys(SiteSection(QStringLiteral("s"))).size() == 2;
  };
  CHECK(Value("PortNumber=22"));
  CHECK(!Value("PortNumber=99999"));
  CHECK(!Value("FSProtocol=3"));
  CHECK(Value("FSProtocol=7"));
  CHECK(!Value("Ftps=9"));
  const QByteArray Sha = "SHA256:abcdefghijklmnopqrstuvwxyz0123456789ABCDEFG";
  CHECK(!Value("SshHostKey=*"));
  CHECK(!Value("SshHostKey=acceptnew"));
  CHECK(!Value("SshHostKey=%20ACCEPTNEW%20"));
  CHECK(!Value("SshHostKey=" + Sha + ";*"));
  CHECK(!Value("SshHostKey=" + Sha + ";acceptnew"));
  CHECK(!Value("SshHostKey=ssh-rsa 2048 AA"));
  CHECK(!Value("SshHostKey=SHA256:short"));
  CHECK(Value("SshHostKey=" + Sha));
  CHECK(Value("SshHostKey=ssh-ed25519 255 " + Sha));
  CHECK(Value("SshHostKey=ecdsa-sha2-nistp256 256 " + Sha + ";" + Sha));
  CHECK(Value("SshHostKey=ssh-rsa 2048 aa:bb:cc:dd:ee:ff:00:11:22:33:44:55:66:77:88:99"));
  CHECK(!Value("UserName=$HOME"));
  CHECK(!Value("UserName=%25USERNAME%25"));
  CHECK(!Value("UserName=~root"));
  CHECK(Value("UserName=deploy"));
  CHECK(Value("Note=one%0Atwo"));
  CHECK(!Value("RemoteDirectory=/a%0Ab"));
  // personal settings survive, shared ones come from the team, defaults reset
  SessionData Local;
  Local.Name = QStringLiteral("x");
  Local.HostName = QStringLiteral("old.example");
  Local.RemoteDirectory = QStringLiteral("/old");
  Local.AgentAccess = 2;
  Local.AgentName = QStringLiteral("mine");
  Local.PublicKeyFile = QStringLiteral("/home/me/key");
  Local.ProxyMethod = pmCmd;
  Local.ProxyLocalCommand = QStringLiteral("nc %host %port");
  Local.ProxyPassword = QStringLiteral("proxy-pw");
  Local.ProxyUsername = QStringLiteral("pu");
  Local.ProxyHost = QStringLiteral("ph");
  IniFile Team;
  SessionData Remote;
  Remote.HostName = QStringLiteral("new.example");
  Remote.UserName = QStringLiteral("deploy");
  Remote.Password = QStringLiteral("team-pw");
  CHECK(AddSite(Team, QStringLiteral("x"), Remote));
  auto Merged = Merge(Team, SiteSection(QStringLiteral("x")), &Local, QStringLiteral("Team: T/x"));
  CHECK((Merged->Name == QStringLiteral("Team: T/x")) && (Merged->HostName == QStringLiteral("new.example")) && (Merged->Password == QStringLiteral("team-pw")));
  CHECK(Merged->RemoteDirectory.isEmpty());
  // sites from a team never expand variables, whatever the team sent
  IniFile Stored;
  {
    IniStorage Storage(&Stored);
    Storage.OpenSubKey(QStringLiteral("Site"), true);
    Merged->Save(Storage, nullptr);
  }
  CHECK(Stored.ReadRaw(QStringLiteral("Site"), QStringLiteral("NoExpansion")) == QStringLiteral("1"));
  CHECK(!IsSharedKey(QStringLiteral("NoExpansion")));
  CHECK((Merged->AgentAccess == 2) && (Merged->AgentName == QStringLiteral("mine")) && (Merged->PublicKeyFile == QStringLiteral("/home/me/key")));
  CHECK((Merged->ProxyMethod == pmCmd) && (Merged->ProxyLocalCommand == QStringLiteral("nc %host %port")) && (Merged->ProxyPassword == QStringLiteral("proxy-pw")));
  auto Fresh = Merge(Team, SiteSection(QStringLiteral("x")), nullptr, QStringLiteral("Team: T/x"));
  CHECK((Fresh->ProxyMethod == pmNone) && (Fresh->AgentAccess == 0));
}

int main(int argc, char ** argv)
{
  QCoreApplication App(argc, argv);
  using namespace TeamCrypto;
  const QString Team = QStringLiteral("6f1c0b4e-2d8a-4c55-9a1e-0b7f2c3d4e5f");

  // key pairs
  KeyPair Alice = NewKeyPair();
  KeyPair Bob = NewKeyPair();
  CHECK((Alice.Public.size() == 32) && (Alice.Private.size() == 32));
  CHECK(PublicKey(Alice.Private) == Alice.Public);
  // keys nothing can be wrapped for
  CHECK(IsUsableBoxKey(Alice.Public));
  for (const char * Hex : {"0000000000000000000000000000000000000000000000000000000000000000",
         "0100000000000000000000000000000000000000000000000000000000000000",
         "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800",
         "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f1157",
         "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f"})
  {
    CHECK(!IsUsableBoxKey(QByteArray::fromHex(Hex)));
  }
  CHECK(!IsUsableBoxKey(QByteArray()));
  CHECK(!IsUsableBoxKey(QByteArray(31, 'x')));
  CHECK(Alice.Public != Bob.Public);

  // identity keys
  SignKey Identity = NewSignKey();
  SignKey Other = NewSignKey();
  CHECK(Identity.IsValid() && (Identity.Public != Other.Public));
  CHECK(SignKeyFromSeed(Identity.Seed).Public == Identity.Public);
  CHECK(!SignKeyFromSeed(QByteArray(31, 'x')).IsValid());
  QByteArray Signature = Sign(Identity, "message");
  CHECK(Signature.size() == SignatureLength);
  CHECK(Verify(Identity.Public, "message", Signature));
  CHECK(!Verify(Identity.Public, "massage", Signature));
  CHECK(!Verify(Other.Public, "message", Signature));
  CHECK(!Verify(Identity.Public, "message", QByteArray()));
  CHECK(!Verify(QByteArray(), "message", Signature));
  CHECK(!Verify(QByteArray(32, '\0'), "message", QByteArray(64, '\0')));
  // RFC 8032 test 1
  SignKey Rfc = SignKeyFromSeed(QByteArray::fromHex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"));
  CHECK(Rfc.Public.toHex() == "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
  CHECK(Sign(Rfc, QByteArray()).toHex() == "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
    "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
  // fingerprints: the same for everyone, different per key, short enough to compare
  QString Fingerprint1 = Fingerprint(Identity.Public);
  CHECK(Fingerprint1.size() == 29);
  CHECK(Fingerprint1 == Fingerprint(Identity.Public));
  CHECK(Fingerprint1 != Fingerprint(Other.Public));
  CHECK(Fingerprint(Rfc.Public) == Fingerprint(Rfc.Public));

  // the private halves under the sync DEK
  QByteArray Dek = CloudCrypto::RandomBytes(32);
  QByteArray Blob = KeyBlob(Alice, Identity);
  QByteArray Wrapped = WrapPrivateKey(Dek, Blob);
  CHECK(UnwrapPrivateKey(Dek, Wrapped) == Blob);
  // the layout CloudSync::RewrapUserKeys relies on: X25519 private, Ed25519 seed, no AAD
  CHECK(Blob == Alice.Private + Identity.Seed);
  QByteArray Opened64;
  CHECK(CloudCrypto::Open(Dek, Wrapped, QByteArray(), Opened64) && (Opened64 == Blob));
  CHECK(UnwrapPrivateKey(CloudCrypto::RandomBytes(32), Wrapped).isEmpty());
  KeyPair Box;
  SignKey Split;
  CHECK(SplitKeyBlob(Blob, Box, Split));
  CHECK((Box.Public == Alice.Public) && (Box.Private == Alice.Private) && (Split.Public == Identity.Public));
  CHECK(!SplitKeyBlob(Alice.Private, Box, Split));

  // member wraps: only the member, for this team and key version
  QByteArray TeamKey = NewTeamKey();
  QByteArray ForBob = WrapForMember(Bob.Public, TeamKey, Team, 3);
  CHECK(ForBob.size() == 32 + 12 + 32 + 16);
  CHECK(UnwrapForMember(Bob, ForBob, Team, 3) == TeamKey);
  CHECK(UnwrapForMember(Alice, ForBob, Team, 3).isEmpty());
  CHECK(UnwrapForMember(Bob, ForBob, Team, 4).isEmpty());
  CHECK(UnwrapForMember(Bob, ForBob, QStringLiteral("00000000-0000-0000-0000-000000000000"), 3).isEmpty());
  CHECK(UnwrapForMember(Bob, ForBob, Team.toUpper(), 3) == TeamKey);  // the uuid is lowercased
  QByteArray Tampered = ForBob;
  Tampered[40] = static_cast<char>(Tampered.at(40) ^ 1);
  CHECK(UnwrapForMember(Bob, Tampered, Team, 3).isEmpty());
  CHECK(WrapForMember(Bob.Public, TeamKey, Team, 3) != ForBob);  // a new ephemeral key every time

  // invite codes
  Invite Code = NewInvite();
  CHECK((Code.Id.size() == 16) && (Code.Secret.size() == SecretLength));
  QString Text = Code.Code();
  CHECK(Text.size() == 16 + 1 + 32);
  Invite Parsed;
  CHECK(ParseInviteCode(Text, Parsed) && (Parsed.Id == Code.Id) && (Parsed.Secret == Code.Secret));
  // typed: lowercase, spaces, O for 0, I/L for 1
  QString Typed = QStringLiteral("  ") + Text.toLower().replace(QLatin1Char('0'), QLatin1Char('o')).replace(QLatin1Char('1'), QLatin1Char('l')) +
    QStringLiteral(" ");
  CHECK(ParseInviteCode(Typed, Parsed) && (Parsed.Id == Code.Id) && (Parsed.Secret == Code.Secret));
  CHECK(!ParseInviteCode(QStringLiteral("nonsense"), Parsed));
  CHECK(!ParseInviteCode(Code.Id + QStringLiteral("-ABC"), Parsed));
  CHECK(!ParseInviteCode(Code.Id + QStringLiteral("-") + QString(32, QLatin1Char('U')), Parsed));  // U is not Crockford

  // invite wraps and verifiers: the code opens the key, the name and the anchor
  InviteContent Content;
  Content.TeamKey = TeamKey;
  Content.TeamName = QStringLiteral("Żółć team");
  Content.RosterSeq = 5;
  Content.RosterHash = QString(64, QLatin1Char('a'));
  QByteArray InviteWrap = WrapForInvite(Code, Content, Team, 3);
  InviteContent Opened;
  CHECK(UnwrapForInvite(Code, InviteWrap, Team, 3, Opened));
  CHECK((Opened.TeamKey == TeamKey) && (Opened.TeamName == Content.TeamName) && (Opened.RosterSeq == 5) && (Opened.RosterHash == Content.RosterHash));
  CHECK(!UnwrapForInvite(Code, InviteWrap, Team, 2, Opened));
  Invite OtherCode = NewInvite();
  CHECK(!UnwrapForInvite(OtherCode, InviteWrap, Team, 3, Opened));
  CHECK(!InviteWrap.contains("team"));
  // the signing key: from the code alone, never the verifier or the wrap key
  CHECK(InviteSignKey(Code).Public == InviteSignKey(Parsed).Public);
  CHECK(InviteSignKey(Code).Public != InviteSignKey(OtherCode).Public);
  CHECK(InviteSignKey(Code).Seed != InviteVerifier(Code));
  QByteArray V = InviteVerifier(Code);
  CHECK(V.size() == 32);
  CHECK(V != HkdfSha256(Code.Secret, Code.Id.toLatin1(), QByteArray("NXT1-INVITE|") + Team.toUtf8() + "|3"));
  CHECK(InviteVerifierHash(Code).size() == 64);
  CHECK(InviteVerifierHash(Code) == InviteVerifierHash(Parsed));
  CHECK(InviteVerifierHash(Code) != InviteVerifierHash(OtherCode));

  // HKDF-SHA256 against RFC 5869 test case 1
  QByteArray Okm = HkdfSha256(QByteArray(22, '\x0b'), QByteArray::fromHex("000102030405060708090a0b0c"),
    QByteArray::fromHex("f0f1f2f3f4f5f6f7f8f9"), 42);
  CHECK(Okm.toHex() == "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");

  // team snapshots: signed by the author over header and ciphertext
  Snapshot Header;
  Header.Team = Team;
  Header.Version = 7;
  Header.KeyVersion = 3;
  Header.Author = QStringLiteral("aaaaaaaa-0000-4000-8000-000000000001");
  Header.RosterSeq = 12;
  Header.Previous = QString(64, QLatin1Char('b'));
  Header.SavedAt = QDateTime::currentDateTimeUtc();
  QByteArray Data = EncodeSnapshot(Header, TeamKey, Identity, "[Sessions\\Team%20Site]\nHostName=x\n");
  Snapshot Read;
  QByteArray Plain;
  CHECK(ReadSnapshot(Data, Read));
  CHECK((Read.Team == Team) && (Read.Version == 7) && (Read.KeyVersion == 3) && (Read.Author == Header.Author) &&
    (Read.RosterSeq == 12) && (Read.Previous == Header.Previous));
  CHECK(VerifySnapshot(Data, Identity.Public));
  CHECK(!VerifySnapshot(Data, Other.Public));
  CHECK(DecodeSnapshot(Data, TeamKey, Plain) && Plain.contains("HostName=x"));
  CHECK(!DecodeSnapshot(Data, NewTeamKey(), Plain));
  CHECK(!Data.contains("HostName"));
  // every byte is covered: header, ciphertext, signature
  for (qsizetype Position : {qsizetype(9), qsizetype(40), Data.size() / 2, Data.size() - 70, Data.size() - 1})
  {
    QByteArray Bad = Data;
    Bad[Position] = static_cast<char>(Bad.at(Position) ^ 1);
    CHECK(!VerifySnapshot(Bad, Identity.Public));
  }
  // the author's signature cannot be moved to other bytes under the same key
  CHECK(!ReadSnapshot(QByteArray("NXS1aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"), Read));
  CHECK(!ReadSnapshot(QByteArray(), Read));
  // a snapshot that claims to expand to gigabytes is refused unread
  {
    QByteArray Bomb = EncodeSnapshot(Header, TeamKey, Identity, QByteArray(1024, 'x'));
    QByteArray Prefix = Bomb.left(8 + qFromLittleEndian<quint32>(Bomb.constData() + 4));
    QByteArray Compressed = qCompress(QByteArray(1024, 'x'));
    Compressed[0] = '\x7F';
    QByteArray Forged = Prefix + CloudCrypto::Seal(TeamKey, Compressed, Prefix);
    Forged += Sign(Identity, QByteArrayLiteral("NXT2-SNAPSHOT|") + Forged);
    QString Error;
    CHECK(!DecodeSnapshot(Forged, TeamKey, Plain, &Error));
    CHECK(Error.contains(QStringLiteral("too large")));
  }
  // empty content works in the packed form
  CHECK(DecodeSnapshot(EncodeSnapshot(Header, TeamKey, Identity, QByteArray()), TeamKey, Plain) && Plain.isEmpty());

  TestRoster();
  TestContent();

  if (Failures == 0)
  {
    std::printf("All team crypto tests passed\n");
  }
  return (Failures == 0) ? 0 : 1;
}
