// End-to-end encryption of NixSCP Teams (nixscp-cloud docs/TEAMS.md,
// "Client-side crypto contract"). The server only stores what this produces.
//
// - Every user has an X25519 key pair; the private key is stored on the server
//   encrypted with the user's sync DEK.
// - A team has a random 32-byte team key per key version, given to each member
//   as an X25519 sealed box bound to the team and the key version.
// - An invite code "<id>-<secret>" carries a secret the server never sees: the
//   team key is wrapped with a key derived from it, and a separate verifier
//   proves knowledge of it when accepting.
#pragma once

#include <QByteArray>
#include <QString>

namespace TeamCrypto
{
  constexpr int KeyLength = 32;
  constexpr int SecretLength = 20;

  struct KeyPair
  {
    QByteArray Public;   // 32 bytes
    QByteArray Private;  // 32 bytes
  };

  QByteArray HkdfSha256(const QByteArray & Ikm, const QByteArray & Salt, const QByteArray & Info, int Length = KeyLength);

  KeyPair NewKeyPair();
  // The public key of a private key (empty on failure)
  QByteArray PublicKey(const QByteArray & Private);
  // wrapped_private_key: AES-256-GCM under the sync DEK (nonce || ct || tag)
  QByteArray WrapPrivateKey(const QByteArray & Dek, const QByteArray & Private);
  QByteArray UnwrapPrivateKey(const QByteArray & Dek, const QByteArray & Wrapped);

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
  QByteArray WrapForInvite(const Invite & Code, const QByteArray & TeamKey, const QString & TeamUuid, qint64 KeyVersion);
  QByteArray UnwrapForInvite(const Invite & Code, const QByteArray & Wrap, const QString & TeamUuid, qint64 KeyVersion);
  QByteArray InviteVerifier(const Invite & Code);
  // hex(SHA-256(verifier))
  QString InviteVerifierHash(const Invite & Code);
}
