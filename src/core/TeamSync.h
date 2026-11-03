// NixSCP Teams (nixscp-cloud docs/TEAMS.md): site folders shared between
// accounts, end-to-end encrypted (core/TeamCrypto). A team's sites appear in
// the site tree as "Team: <team name>/..."; owners and admins edit them (the
// changes are pushed), members use them (local changes are reverted). Removed
// members lose the folder; the team key is rotated by the next admin.
//
// Needs NixSCP Pro (the beta counts) with cloud sync unlocked: the user's team
// key pair is stored on the server encrypted with the sync data key.
#pragma once

#include "core/TeamCrypto.h"

#include <QHash>
#include <QJsonArray>
#include <QObject>

#include <functional>

class Configuration;
class CloudSync;
class IniFile;

class TeamSync : public QObject
{
  Q_OBJECT

public:
  using Done = std::function<void(bool Ok, const QString & Error)>;

  struct Team
  {
    QString Uuid;
    QString Name;
    QString Role;          // owner, admin, member
    qint64 KeyVersion = 0;
    QByteArray Key;        // empty: not available (yet)
    bool NeedsRotation = false;
    int MemberCount = 0;
    qint64 HeadVersion = 0;
    bool CanEdit() const { return (Role == QStringLiteral("owner")) || (Role == QStringLiteral("admin")); }
  };

  static TeamSync & Instance();
  void Initialize(Configuration & Config, CloudSync & Sync);

  // "Team: <name>"
  static QString FolderName(const QString & TeamName);
  // The team of a stored site name ("Team: X/site"), nullptr when none
  const Team * TeamOfSite(const QString & SiteName) const;

  QList<Team> Teams() const { return FTeams.values(); }
  const Team * Find(const QString & Uuid) const;
  bool Available() const;   // signed in with the sync key
  QString LastError() const { return FLastError; }

  // Full sync of all teams (keys, rotation, content)
  void SyncAll(const Done & Callback = {});

  void CreateTeam(const QString & Name, const Done & Callback);
  void Rename(const QString & Uuid, const QString & Name, const Done & Callback);
  void DeleteTeam(const QString & Uuid, const Done & Callback);
  void Leave(const QString & Uuid, const Done & Callback);
  // Moves the sites of a local folder into the team (as a subfolder) and pushes
  void ShareFolder(const QString & Uuid, const QString & Folder, const Done & Callback);
  // Creates an invite; Callback gets the code to hand over ("<id>-<secret>")
  void CreateInvite(const QString & Uuid, const QString & Role, int Days,
    const std::function<void(bool Ok, const QString & Error, const QString & Code)> & Callback);
  void Invites(const QString & Uuid, const std::function<void(bool Ok, const QString & Error, const QJsonArray & List)> & Callback);
  void RevokeInvite(const QString & Uuid, const QString & Id, const Done & Callback);
  void JoinWithCode(const QString & Code, const Done & Callback);
  void Members(const QString & Uuid, const std::function<void(bool Ok, const QString & Error, const QJsonArray & List)> & Callback);
  void SetRole(const QString & Uuid, const QString & UserUuid, const QString & Role, const Done & Callback);
  // Removes a member; the key is rotated and the content pushed again
  void RemoveMember(const QString & Uuid, const QString & UserUuid, const Done & Callback);

  // The team content of the local sites (canonical, for hashing and pushing)
  QByteArray LocalContent(const Team & T) const;

signals:
  void Changed();
  // Something the user should know (removed from a team, changes reverted...)
  void Notice(const QString & Message);

private:
  TeamSync() = default;
  Configuration * FConfig = nullptr;
  CloudSync * FSync = nullptr;
  TeamCrypto::KeyPair FKeys;
  QString FKeysUser;
  QHash<QString, Team> FTeams;
  QString FLastError;
  bool FSyncing = false;
  QList<Done> FWaiting;

  // per team, persisted: base version, hash of the content synced last, folder name
  struct State
  {
    qint64 Base = 0;
    QString Hash;
    QString Folder;
  };
  QHash<QString, State> FStates;
  QString StateFileName() const;
  void LoadStates();
  void SaveStates();

  void EnsureKeys(const Done & Callback);
  void FetchTeams(const Done & Callback);
  void SyncTeams(QStringList Pending, const Done & Callback);
  void SyncTeam(const QString & Uuid, const Done & Callback);
  void Rotate(const QString & Uuid, const Done & Callback);
  void Pull(const QString & Uuid, qint64 Version, const Done & Callback);
  void Push(const QString & Uuid, const Done & Callback);
  void Apply(Team & T, const QByteArray & Content);
  void RemoveLocal(const QString & Folder);
  void Finish(bool Ok, const QString & Error);
};
