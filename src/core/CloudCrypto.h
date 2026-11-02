// End-to-end encryption of NixSCP Pro sync snapshots (nixscp-cloud docs/SYNC.md,
// "Encrypted snapshot format"). Only ciphertext ever leaves the computer:
//
//   magic "NXS1" | header_len (LE32) | header (JSON) | nonce (12) | ciphertext | tag (16)
//
// The payload is AES-256-GCM(DEK, qCompress(plaintext)) with the first
// 8 + header_len bytes as additional authenticated data, so the server cannot
// change the slot, user or version of a snapshot without the client noticing.
// The random data key (DEK) travels inside every header, wrapped (AES-256-GCM)
// with a key derived from the sync password (Argon2id) and, optionally, with a
// recovery key. A wrong password fails the GCM tag check of the wrap.
#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>

namespace CloudCrypto
{
  constexpr int KeyLength = 32;

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
    int Format = 1;
    QString Slot;          // "settings" or "session"
    QString User;          // account uuid
    qint64 Version = 0;    // head version this snapshot becomes (base_version + 1)
    QDateTime SavedAt;     // UTC, when the plaintext was captured
    KdfParams Kdf;
    QByteArray DekPassword;  // nonce || AES-256-GCM(KEK, DEK) || tag
    QByteArray DekRecovery;  // same, wrapped with the recovery key; empty = no recovery key
    QString Device;          // informational
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
  // The DEK, or empty when the password (or recovery key) is wrong
  QByteArray UnwrapDekWithPassword(const SnapshotHeader & Header, const QString & Password, QString * Error = nullptr);
  QByteArray UnwrapDekWithRecoveryKey(const SnapshotHeader & Header, const QByteArray & RecoveryKey);

  QByteArray EncodeSnapshot(const SnapshotHeader & Header, const QByteArray & Dek, const QByteArray & Plaintext);
  // Reads the (unencrypted, but authenticated) header without any key
  bool ReadHeader(const QByteArray & Snapshot, SnapshotHeader & Header, QString * Error = nullptr);
  // Decrypts and authenticates the whole snapshot
  bool DecodeSnapshot(const QByteArray & Snapshot, const QByteArray & Dek, SnapshotHeader & Header,
    QByteArray & Plaintext, QString * Error = nullptr);
}
