// End-to-end encryption of NixSCP Pro sync snapshots (nixscp-cloud docs/SYNC.md,
// "Encrypted snapshot format"). Only ciphertext ever leaves the computer:
//
//   magic "NXS1" | header_len (LE32) | header (JSON) | nonce (12) | ciphertext | tag (16)
//
// The payload is AES-256-GCM(DEK, packed plaintext) with the first
// 8 + header_len bytes as additional authenticated data, so the server cannot
// change the slot, user or version of a snapshot without the client noticing.
// The plaintext is packed as be32(total length), then for each 64 KiB block
// be32(size) and qCompress(block): the length is known before anything is
// inflated, and one block cannot expand far (qUncompress alone grows its
// buffer past the length it is told, up to whatever the data inflates to).
// The random data key (DEK) travels inside every header, wrapped (AES-256-GCM)
// with a key derived from the sync password (Argon2id) and, optionally, with a
// recovery key. A wrong password fails the GCM tag check of the wrap, so anyone
// who holds a header can test password guesses offline, at Argon2id's cost.
//
// key_epoch counts the data keys of the account and wrap_epoch every change of
// its wraps, so old wraps never replace newer ones. prev_version and prev_sha256
// name the snapshot this one was made on top of, which lets a device follow the
// chain back to what it saw. A snapshot of a new key_epoch carries "rotated",
// a value only a holder of the previous key can make: a device left with the old
// key can tell that the key was replaced from a snapshot it merely cannot read.
#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>

namespace CloudCrypto
{
  constexpr int KeyLength = 32;
  // Snapshots of other formats are refused
  constexpr int SnapshotFormat = 2;
  // A snapshot never expands beyond this
  constexpr int MaxPlaintextLength = 64 * 1024 * 1024;
  // A download is never larger (the server allows 5 MiB)
  constexpr qint64 MaxSnapshotLength = 16 * 1024 * 1024;
  constexpr int MinPasswordLength = 12;

  struct KdfParams
  {
    // Argon2id; 64 MiB / 3 passes / 1 lane takes ~0.2-0.5 s on a current PC
    quint32 MemoryKib = 64 * 1024;
    quint32 Iterations = 3;
    quint32 Lanes = 1;
    QByteArray Salt;  // 16 random bytes
  };

  struct SnapshotHeader
  {
    int Format = SnapshotFormat;
    QString Slot;          // "settings", "session", "team" or "local" (core/ConfigVault)
    QString User;          // account uuid
    qint64 Version = 0;    // head version this snapshot becomes (base_version + 1)
    QDateTime SavedAt;     // UTC, when the plaintext was captured
    KdfParams Kdf;
    QByteArray DekPassword;  // nonce || AES-256-GCM(KEK, DEK) || tag
    QByteArray DekRecovery;  // same, wrapped with the recovery key; empty = no recovery key
    QString Device;          // informational
    QString Role;            // slot "local": what the file is (ConfigVault), so files cannot stand in for each other
    QString ConfigName;      // slot "local", role "state": the name of the configuration file this state goes with
    qint64 KeyEpoch = 0;     // which data key of the account this is
    qint64 WrapEpoch = 0;    // how often the wraps of the account changed
    QByteArray Rotated;      // RotationProof of this KeyEpoch; empty on the first key
    qint64 PrevVersion = 0;  // the snapshot of this slot this one follows, 0 = none
    QString PrevHash;        // its lower-case hex SHA-256
    // Team snapshots (slot "team", nixscp-cloud docs/TEAMS.md): User is the team
    // uuid, the key is the team key (no kdf/dek_pw/dek_rec), KeyVersion its version
    qint64 KeyVersion = 0;
    bool IsTeam() const { return Slot == QStringLiteral("team"); }
  };

  QByteArray RandomBytes(int Length);

  // Argon2id(password); empty on failure (Error set)
  QByteArray DeriveKey(const QString & Password, const KdfParams & Params, QString * Error = nullptr);

  // AES-256-GCM: nonce (12) || ciphertext || tag (16)
  QByteArray Seal(const QByteArray & Key, const QByteArray & Plaintext, const QByteArray & Aad);
  // false when the key or the data is wrong (authentication failure)
  bool Open(const QByteArray & Key, const QByteArray & Sealed, const QByteArray & Aad, QByteArray & Plaintext);

  // Recovery key: 32 random bytes shown as 13 groups of 4 Crockford base32
  // characters ("ABCD-EFGH-..."); typing errors (case, O/0, I/L/1) are tolerated
  QByteArray NewRecoveryKey();
  QString FormatRecoveryKey(const QByteArray & Key);
  // Empty when the text is not a valid recovery key
  QByteArray ParseRecoveryKey(const QString & Text);

  // Fills Kdf (new salt), DekPassword and, when RecoveryKey is not empty,
  // DekRecovery of the header. The wraps are bound to Header.User.
  bool WrapDek(SnapshotHeader & Header, const QByteArray & Dek, const QString & Password,
    const QByteArray & RecoveryKey, QString * Error = nullptr);
  // Replaces DekRecovery only: the password wrap stays
  void WrapDekForRecovery(SnapshotHeader & Header, const QByteArray & Dek, const QByteArray & RecoveryKey);
  // The DEK, or empty when the password (or recovery key) is wrong
  QByteArray UnwrapDekWithPassword(const SnapshotHeader & Header, const QString & Password, QString * Error = nullptr);
  QByteArray UnwrapDekWithRecoveryKey(const SnapshotHeader & Header, const QByteArray & RecoveryKey);

  // Proves to holders of the previous key that the key of NewKeyEpoch replaced it
  QByteArray RotationProof(const QByteArray & OldDek, const QString & User, qint64 NewKeyEpoch);
  // The header (not authenticated: its snapshot cannot be read with OldDek)
  // announces a key that replaced OldDek. Only a holder of OldDek can make this
  // true, which tells such a rotation from a snapshot of a stranger.
  bool ProvesRotation(const SnapshotHeader & Header, const QByteArray & OldDek);

  // Ed25519, for the identity key of Teams (32-byte private seed and public
  // key, 64-byte signature); empty on failure
  QByteArray Ed25519Public(const QByteArray & Private);
  QByteArray Ed25519Sign(const QByteArray & Private, const QByteArray & Message);

  // Strength of a new NixSCP password: every snapshot lets the server test
  // guesses offline, so a short or common one is not protected by anything else.
  // 0 = too weak, 1 = acceptable, 2 = good, 3 = strong
  int PasswordScore(const QString & Password);
  // Why the password cannot be used (it counts common words, keyboard runs and
  // repeats as guessable); empty when it can. Email: the account's address.
  QString PasswordProblem(const QString & Password, const QString & Email = QString());
  // A random passphrase of 150 bits, easy to type: "k7fqm-2xwpd-..."
  QString NewPassphrase();

  // The plaintext as compressed blocks, and back: a hostile input cannot inflate
  // past MaxPlaintextLength; false when it would or when it is damaged
  QByteArray Pack(const QByteArray & Plaintext);
  bool Unpack(const QByteArray & Packed, QByteArray & Plaintext);

  QByteArray EncodeSnapshot(const SnapshotHeader & Header, const QByteArray & Dek, const QByteArray & Plaintext);
  // Reads the (unencrypted, but authenticated) header without any key
  bool ReadHeader(const QByteArray & Snapshot, SnapshotHeader & Header, QString * Error = nullptr);
  // A snapshot of an earlier format (NixSCP 1.2.3 and before wrote format 1): not
  // damaged, but ReadHeader refuses it and nothing here can convert it
  bool IsOldFormat(const QByteArray & Snapshot);
  // Decrypts and authenticates the whole snapshot
  bool DecodeSnapshot(const QByteArray & Snapshot, const QByteArray & Dek, SnapshotHeader & Header,
    QByteArray & Plaintext, QString * Error = nullptr);
}
