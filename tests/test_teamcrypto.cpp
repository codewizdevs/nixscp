// NixSCP Teams end-to-end encryption (core/TeamCrypto): key pairs, member
// wraps bound to team and key version, invite codes, invite wraps and
// verifiers, private keys under the sync DEK, and team snapshot headers.
#include "core/CloudCrypto.h"
#include "core/TeamCrypto.h"

#include <QCoreApplication>

#include <cstdio>

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)

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
  CHECK(Alice.Public != Bob.Public);

  // the private key under the sync DEK
  QByteArray Dek = CloudCrypto::RandomBytes(32);
  QByteArray Wrapped = WrapPrivateKey(Dek, Alice.Private);
  CHECK(UnwrapPrivateKey(Dek, Wrapped) == Alice.Private);
  CHECK(UnwrapPrivateKey(CloudCrypto::RandomBytes(32), Wrapped).isEmpty());

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

  // invite wraps and verifiers
  QByteArray InviteWrap = WrapForInvite(Code, TeamKey, Team, 3);
  CHECK(InviteWrap.size() == 12 + 32 + 16);
  CHECK(UnwrapForInvite(Code, InviteWrap, Team, 3) == TeamKey);
  CHECK(UnwrapForInvite(Code, InviteWrap, Team, 2).isEmpty());
  Invite Other = NewInvite();
  CHECK(UnwrapForInvite(Other, InviteWrap, Team, 3).isEmpty());
  QByteArray V = InviteVerifier(Code);
  CHECK(V.size() == 32);
  CHECK(V != HkdfSha256(Code.Secret, Code.Id.toLatin1(), QByteArray("NXT1-INVITE|") + Team.toUtf8() + "|3"));
  CHECK(InviteVerifierHash(Code).size() == 64);
  CHECK(InviteVerifierHash(Code) == InviteVerifierHash(Parsed));
  CHECK(InviteVerifierHash(Code) != InviteVerifierHash(Other));

  // HKDF-SHA256 against RFC 5869 test case 1
  QByteArray Okm = HkdfSha256(QByteArray(22, '\x0b'), QByteArray::fromHex("000102030405060708090a0b0c"),
    QByteArray::fromHex("f0f1f2f3f4f5f6f7f8f9"), 42);
  CHECK(Okm.toHex() == "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");

  // team snapshots: team key, key version in the header, no password wraps
  CloudCrypto::SnapshotHeader Header;
  Header.Slot = QStringLiteral("team");
  Header.User = Team;
  Header.Version = 7;
  Header.KeyVersion = 3;
  Header.SavedAt = QDateTime::currentDateTimeUtc();
  QByteArray Snapshot = CloudCrypto::EncodeSnapshot(Header, TeamKey, "[Sessions\\Team%20Site]\nHostName=x\n");
  CloudCrypto::SnapshotHeader Read;
  QByteArray Plain;
  CHECK(CloudCrypto::DecodeSnapshot(Snapshot, TeamKey, Read, Plain));
  CHECK(Read.IsTeam() && (Read.User == Team) && (Read.Version == 7) && (Read.KeyVersion == 3) && Plain.contains("HostName=x"));
  CHECK(!Snapshot.contains("dek_pw") && !Snapshot.contains("kdf"));
  CHECK(!CloudCrypto::DecodeSnapshot(Snapshot, NewTeamKey(), Read, Plain));

  if (Failures == 0)
  {
    std::printf("All team crypto tests passed\n");
  }
  return (Failures == 0) ? 0 : 1;
}
