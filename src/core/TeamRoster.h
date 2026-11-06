// The signed member list of a NixSCP team (nixscp-cloud docs/TEAMS.md): an
// append-only, hash-chained log the server stores but cannot write. Replaying
// it from the genesis gives who is in the team, their roles and keys, the key
// version, and whether the key must be renewed. Nothing the server says about
// these is believed. Pure functions: core/TeamSync fetches and pins.
#pragma once

#include "core/TeamCrypto.h"

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>

namespace TeamRoster
{
  struct Entry
  {
    // As stored: the signed bytes and the signatures
    QByteArray Body;
    QByteArray Sig;
    QByteArray Sig2;   // join: by the invite key
    // Parsed from Body
    QString Team;
    qint64 Seq = 0;
    QString Prev;
    QString Type;
    QString Signer;
    QString Time;
    QJsonObject Data;
    QString Hash;      // hex SHA-256 of Body
  };

  // More than a chain may have: the server stops growing one well before this
  constexpr int MaxEntries = 20000;

  struct Member
  {
    QString Uuid;
    QString Email;       // as the member stated it when joining
    QString Role;        // owner, admin, member
    QByteArray Identity; // Ed25519
    QByteArray Box;      // X25519
    bool Usable = true;  // Box can receive a wrap; a rotation covers the others without it
    QString WrapHash;    // hex SHA-256 of the member's team key wrap
    qint64 WrapVersion = 0;
    qint64 JoinedSeq = 0;
    QString Joined;
  };

  struct PendingInvite
  {
    QString Id;
    QString Role;
    QString Expires;
    QByteArray Public;
    qint64 KeyVersion = 0;
  };

  // A role a user got at a roster position; Role empty: no longer in the team
  struct RoleChange
  {
    qint64 Seq = 0;
    QString Role;
    QByteArray Identity;
    qint64 Head = -1;    // the snapshot version the signer knew
  };

  struct State
  {
    QString Team;
    qint64 Seq = 0;
    QString Hash;
    QString Owner;
    qint64 KeyVersion = 0;
    // A member left or was removed after the latest rotate: they know the key
    bool NeedsRotation = false;
    QMap<QString, Member> Members;
    QMap<QString, PendingInvite> Invites;
    QHash<QString, QList<RoleChange>> Roles;
    QList<QPair<qint64, qint64>> Keys;    // roster position, key version from there on
    QHash<qint64, QString> Hashes;        // roster position, entry hash
    const Member * Find(const QString & Uuid) const;
    bool CanEdit(const QString & Uuid) const;
    // The key version in force at a roster position (0: before the genesis)
    qint64 KeyVersionAt(qint64 Seq) const;
    // Did the user write as an owner or admin at Seq? A demotion or removal after
    // Seq closes that, for snapshots after the head version the signer knew then
    bool AuthorAllowed(const QString & Uuid, qint64 Seq, qint64 Version, QByteArray * Identity) const;
  };

  // The wire form {body, sig, sig2}
  bool Parse(const QJsonObject & Wire, Entry & Result, QString * Error = nullptr);
  QJsonObject ToWire(const Entry & Item);

  // A new entry after S, signed by Key and, for a join, by Second as well
  Entry Make(const State & S, const QString & Team, const QString & Type, const QString & Signer, const QJsonObject & Data,
    const TeamCrypto::SignKey & Key, const TeamCrypto::SignKey * Second = nullptr);
  // Verifies the entry as the next one of S (chain, signatures, rules) and
  // applies it. On false, S is unchanged.
  bool Apply(State & S, const Entry & Item, QString * Error = nullptr);
  // Verifies a whole chain from the genesis
  bool Replay(const QJsonArray & Wire, const QString & Team, State & Result, QList<Entry> * Items = nullptr, QString * Error = nullptr);

  QJsonObject CreateData(const QString & Email, const TeamCrypto::SignKey & Identity, const QString & UserUuid,
    const QByteArray & Box, const QByteArray & Wrap);
  QJsonObject JoinData(const QString & InviteId, const QString & Email, const TeamCrypto::SignKey & Identity, const QString & UserUuid,
    const QByteArray & Box, const QByteArray & Wrap);
  QString WrapHash(const QByteArray & Wrap);
  QString NowText();
  // lowercase uuid in the 8-4-4-4-12 form
  bool IsUuid(const QString & Text);
}
