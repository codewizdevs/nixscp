// End-to-end encryption of NixSCP Teams (nixscp-cloud docs/TEAMS.md,
// "Client-side crypto contract"). The server only stores what this produces.
//
// - Every user has an X25519 key pair and an Ed25519 identity key that signs it;
//   the private halves are stored on the server encrypted with the sync DEK.
// - A team has a random 32-byte team key per key version, given to each member
//   as an X25519 sealed box bound to the team and the key version. A sealed box
//   has no sender: the hash of each wrap is in the signed roster (core/TeamRoster).
// - An invite code "<id>-<secret>" carries a secret the server never sees: the
//   team key and the roster hash are wrapped with a key derived from it, a
//   separate verifier proves knowledge of it, and a third derived key signs the
//   join entry.
// - Team snapshots are signed by their author over header and ciphertext.
#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>

namespace TeamCrypto
{
  constexpr int KeyLength = 32;
  constexpr int SecretLength = 20;
  constexpr int SignatureLength = 64;

  struct KeyPair
  {
    QByteArray Public;   // 32 bytes
    QByteArray Private;  // 32 bytes
  };

  // Ed25519
  struct SignKey
  {
    QByteArray Public;   // 32 bytes
    QByteArray Seed;     // 32 bytes, the private key
    bool IsValid() const { return (Public.size() == 32) && (Seed.size() == 32); }
  };

  QByteArray HkdfSha256(const QByteArray & Ikm, const QByteArray & Salt, const QByteArray & Info, int Length = KeyLength);

  KeyPair NewKeyPair();
  // The public key of a private key (empty on failure)
  QByteArray PublicKey(const QByteArray & Private);
  // Something can be wrapped for this key: 32 bytes and not a low-order point
  bool IsUsableBoxKey(const QByteArray & Public);
  // wrapped_private_key: AES-256-GCM under the sync DEK (nonce || ct || tag) of
  // KeyBlob: the X25519 private key followed by the Ed25519 seed
  QByteArray WrapPrivateKey(const QByteArray & Dek, const QByteArray & Blob);
  QByteArray UnwrapPrivateKey(const QByteArray & Dek, const QByteArray & Wrapped);
  QByteArray KeyBlob(const KeyPair & Box, const SignKey & Identity);
  bool SplitKeyBlob(const QByteArray & Blob, KeyPair & Box, SignKey & Identity);

  SignKey NewSignKey();
  // Empty when the seed is not 32 bytes
  SignKey SignKeyFromSeed(const QByteArray & Seed);
  QByteArray Sign(const SignKey & Key, const QByteArray & Message);
  // Refuses anything but a 32-byte key and a 64-byte signature
  bool Verify(const QByteArray & Public, const QByteArray & Message, const QByteArray & Signature);
  // What people compare: 24 base32 characters of SHA-256 of the identity key, "ABCD-EFGH-..."
  QString Fingerprint(const QByteArray & IdentityPublic);
  // What the identity key signs for the box key
  QByteArray BoxKeyMessage(const QString & UserUuid, const QByteArray & BoxPublic);

  QByteArray NewTeamKey();
  // Member wrap: eph_pub32 || nonce12 || ciphertext32 || tag16
  QByteArray WrapForMember(const QByteArray & MemberPublic, const QByteArray & TeamKey, const QString & TeamUuid, qint64 KeyVersion);
  // Empty when the wrap is not for this key pair, team or version
  QByteArray UnwrapForMember(const KeyPair & Mine, const QByteArray & Wrap, const QString & TeamUuid, qint64 KeyVersion);

  // Invite codes: id = 16 Crockford base32 characters, secret = 20 bytes
  struct Invite
  {
    QString Id;
    QByteArray Secret;
    QString Code() const;   // "<id>-<32 base32 characters>"
  };
  Invite NewInvite();
  // Tolerates lowercase, spaces, O/0, I/L/1; false when malformed
  bool ParseInviteCode(const QString & Text, Invite & Result);
  // What the code opens: the join anchor. Only someone with the code can write
  // this, so the roster hash authenticates the genesis key to the joiner.
  struct InviteContent
  {
    QByteArray TeamKey;
    QString TeamName;
    qint64 RosterSeq = 0;
    QString RosterHash;   // hex, of the invite entry at RosterSeq
  };
  QByteArray WrapForInvite(const Invite & Code, const InviteContent & Content, const QString & TeamUuid, qint64 KeyVersion);
  bool UnwrapForInvite(const Invite & Code, const QByteArray & Wrap, const QString & TeamUuid, qint64 KeyVersion, InviteContent & Content);
  QByteArray InviteVerifier(const Invite & Code);
  // hex(SHA-256(verifier))
  QString InviteVerifierHash(const Invite & Code);
  // Signs the join entry; the public half is in the invite entry
  SignKey InviteSignKey(const Invite & Code);

  // Team snapshot:
  //   magic "NXT2" | header_len (LE32) | header (JSON) | nonce (12) | ciphertext | tag (16) | signature (64)
  // AES-256-GCM(team key, qCompress(plaintext)) with the bytes up to the nonce as
  // additional data; Ed25519 by the author over "NXT2-SNAPSHOT|" and all the bytes before it
  struct Snapshot
  {
    QString Team;
    qint64 Version = 0;      // head version this snapshot becomes
    qint64 KeyVersion = 0;
    QString Author;          // user uuid
    qint64 RosterSeq = 0;    // the author's verified roster
    QString Previous;        // hex SHA-256 of the snapshot it replaces; empty for the first
    QDateTime SavedAt;
  };
  QByteArray EncodeSnapshot(const Snapshot & Header, const QByteArray & TeamKey, const SignKey & Author, const QByteArray & Plaintext);
  // The header, unauthenticated until VerifySnapshot and DecodeSnapshot passed
  bool ReadSnapshot(const QByteArray & Data, Snapshot & Header, QString * Error = nullptr);
  bool VerifySnapshot(const QByteArray & Data, const QByteArray & AuthorPublic);
  bool DecodeSnapshot(const QByteArray & Data, const QByteArray & TeamKey, QByteArray & Plaintext, QString * Error = nullptr);
}
