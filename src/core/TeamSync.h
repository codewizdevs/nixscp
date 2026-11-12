// NixSCP Teams (nixscp-cloud docs/TEAMS.md): site folders shared between
// accounts, end-to-end encrypted (core/TeamCrypto). The server is trusted for
// nothing: who is in a team, their keys and roles come from the signed roster
// (core/TeamRoster), the sites and the team's name from snapshots signed by an
// owner or admin (core/TeamContent). A team that fails verification is not
// synced at all, and its local sites stay as they are.
//
// A team's sites are in a local folder chosen once per team uuid ("Team: <team
// name>", with a suffix on a collision). Owners and admins edit them (the
// changes are pushed), members use them (local changes are reverted). A push
// happens only on top of the head this computer knows; when both changed, the
// local edit stays and the admin decides (ResolveConflict).
//
// Needs NixSCP Pro (the beta counts) with cloud sync unlocked: the user's key
// pairs are stored on the server encrypted with the sync data key.
#pragma once

#include "core/CloudClient.h"
#include "core/Security.h"
#include "core/TeamCrypto.h"
#include "core/TeamRoster.h"

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QObject>
#include <QSet>

#include <functional>

class Configuration;
class CloudSync;
class IniFile;
class SessionData;

class TeamSync : public QObject
{
  Q_OBJECT

public:
  using Done = std::function<void(bool Ok, const QString & Error)>;
  using ListDone = std::function<void(bool Ok, const QString & Error, const QJsonArray & List)>;

  struct Team
  {
    QString Uuid;
    QString Name;
    QString Folder;        // the local folder; empty until the team's name is known
    QString Role;          // owner, admin, member (from the verified roster)
    qint64 KeyVersion = 0;
    SecureBytes Key;       // empty: not available
    bool NeedsRotation = false;
    int MemberCount = 0;
    qint64 HeadVersion = 0;
    QString HeadSha;
    QString Error;         // failed verification: not synced, nothing touched
    QString Conflict;      // waits for ResolveConflict
    bool CanEdit() const { return (Role == QStringLiteral("owner")) || (Role == QStringLiteral("admin")); }
  };

  static TeamSync & Instance();
  void Initialize(Configuration & Config, CloudSync & Sync);

  // "Team: <name>": the folder a team of that name would like to have
  static QString FolderName(const QString & TeamName);
  // The team of a stored site name ("Team: X/site"), nullptr when none
  const Team * TeamOfSite(const QString & SiteName) const;

  QList<Team> Teams() const { return FTeams.values(); }
  // The pointer is good until the event loop runs: copy what a dialog keeps
  const Team * Find(const QString & Uuid) const;
  bool Available() const;   // signed in with the sync key
  QString LastError() const { return FLastError; }
  // This account's identity, for comparing with colleagues (empty before the first sync)
  QString Fingerprint() const;

  // Full sync of all teams (keys, roster, rotation, content)
  void SyncAll(const Done & Callback = {});

  void CreateTeam(const QString & Name, const Done & Callback);
  void Rename(const QString & Uuid, const QString & Name, const Done & Callback);
  // Owner: for everyone, with a signed entry. It needs no verified member list:
  // a team whose list fails verification can still be deleted
  void DeleteTeam(const QString & Uuid, const Done & Callback);
  void Leave(const QString & Uuid, const Done & Callback);
  // The way out of a team that cannot be verified, for anyone and with no server:
  // it leaves this computer and stays away until an invite brings it back.
  // KeepSites: its sites stay as a plain folder of this person's
  void Forget(const QString & Uuid, bool KeepSites, const Done & Callback);
  // Shares the sites of a local folder with the team (as a subfolder); they
  // leave the folder once the team has them
  void ShareFolder(const QString & Uuid, const QString & Folder, const Done & Callback);
  // The team's shared sites, relative to its folder ("sub/site"), sorted
  QStringList SharedSites(const QString & Uuid) const;
  // Shares personal sites (stored names) with the team, under Subfolder when
  // given; Move: the personal copies go once the team has them
  void ShareSites(const QString & Uuid, const QStringList & Names, const QString & Subfolder, bool Move, const Done & Callback);
  // Stops sharing team sites (names relative to the team's folder; "sub/"
  // = everything in a subfolder): the members lose them at their next sync.
  // KeepCopy: they move to this person's own sites; otherwise deleted. Pushes.
  void Unshare(const QString & Uuid, const QStringList & Names, bool KeepCopy, const Done & Callback);
  // Creates an invite; Callback gets the code to hand over ("<id>-<secret>")
  void CreateInvite(const QString & Uuid, const QString & Role, int Days,
    const std::function<void(bool Ok, const QString & Error, const QString & Code)> & Callback);
  void Invites(const QString & Uuid, const ListDone & Callback);
  void RevokeInvite(const QString & Uuid, const QString & Id, const Done & Callback);
  void JoinWithCode(const QString & Code, const Done & Callback);
  // From the verified roster: {user_uuid, email, role, joined_at, fingerprint, gone, unusable}.
  // gone: the server no longer has the account (only an admin's removal ends its membership);
  // unusable: nothing can be wrapped for their key, so they get no new team key
  void Members(const QString & Uuid, const ListDone & Callback);
  void SetRole(const QString & Uuid, const QString & UserUuid, const QString & Role, const Done & Callback);
  // Removes a member; the key is rotated and the content pushed again
  void RemoveMember(const QString & Uuid, const QString & UserUuid, const Done & Callback);

  // Team::Conflict: KeepLocal pushes this computer's sites over the head,
  // otherwise they are replaced by the head
  void ResolveConflict(const QString & Uuid, bool KeepLocal, const Done & Callback);
  // Owners and admins: the versions the server keeps,
  // [{version, size, sha256, key_version, created_at, user_email, device}], newest first
  void History(const QString & Uuid, const ListDone & Callback);
  // Makes a verified older version the team's content (a new version on top)
  void Restore(const QString & Uuid, qint64 Version, const Done & Callback);

  // The team's sites on this computer (canonical, reduced to what is shared)
  QByteArray LocalContent(const Team & T) const;

  // ---- Notes ----
  // The note of a site as this person sees it. On a team site it is the team's
  // note (Shared), or, when the team shares none, this person's private one. With
  // team edits on, the team's note is in the team's note channel: written by
  // admins and members alike, each one signed by its author.
  struct NoteView
  {
    QString Text;
    bool InTeam = false;      // a team's site (or note item)
    bool Shared = false;      // the team's note, not this person's private one
    bool TeamEdit = false;    // members edit it too
    bool CanEdit = false;     // this person may change it
    bool CanShare = false;    // owner or admin: sharing and team edits
    QString EditedBy;         // e-mail of who wrote it last, empty when unknown
    QDateTime EditedAt;
  };
  NoteView NoteOf(const SessionData & Site) const;
  // Saves the note this person sees (NoteView): the site's own, the private one,
  // the team's in the content (owners and admins; pushed) or in the note channel
  void SaveNote(const QString & SiteName, const QString & Text, const Done & Callback);
  // Owners and admins. Not shared: the team's note becomes this person's private
  // one and is gone for the others; shared: the private note becomes the team's
  void SetNoteShared(const QString & SiteName, bool Shared, const Done & Callback);
  // Owners and admins: members may edit the note too (off: it goes back into the
  // signed sites, with who wrote it last)
  void SetNoteTeamEdit(const QString & SiteName, bool On, const Done & Callback);

signals:
  void Changed();
  // Something the user should know (removed from a team, changes reverted, a
  // new member, a failed verification...)
  void Notice(const QString & Message);
  // A sync brought changes of a team's sites (from another person or computer)
  void SitesReceived(const QString & TeamName, int Added, int Removed, int Changed);
  // A stored site changed its name (shared, moved back, the team renamed):
  // what is kept per site name elsewhere follows
  void SiteMoved(const QString & From, const QString & To);

private:
  TeamSync() = default;

  // Per account and team, persisted in the sealed nixscp-teams.ini
  struct State
  {
    qint64 Base = 0;           // snapshot version the local sites are based on
    QString Hash;              // hash of the sites at Base
    QHash<QString, QString> BaseSites;   // their names and hashes
    QHash<QString, QString> BaseCritical;   // folded name, where the site connects
    QString Folder;            // authoritative: the name never selects it
    QString Name;
    QString Role;              // as the roster said last, for a team that fails verification
    QString PendingName;       // renamed here, not pushed yet
    bool NeedsPush = false;    // a first snapshot or a decision to push is waiting
    bool AllowEmpty = false;   // the user removed the last sites on purpose
    QHash<QString, QString> Moves;   // personal site to delete, its name in the team
    qint64 ConflictVersion = 0;      // the head the conflict was told for
    bool EmptyConflict = false;
    // Highest verified state: anything below it is a rollback
    qint64 PinVersion = 0;
    QString PinSha;
    qint64 PinKeyVersion = 0;
    qint64 RosterSeq = 0;
    QString RosterHash;
    QHash<qint64, QByteArray> OldKeys;   // team keys under the data key, for the history
    // Off this computer (left, removed, gone): the pins above stay as a floor, as
    // a server answer must never let the team come back below them
    bool Detached = false;
    bool Ignored = false;      // given up here; an invite brings it back, the server does not
    // The note channel: by NoteId, the note accepted last (encrypted and signed as
    // it came; accepted when its author was in the team) and the highest version
    // seen, below which the server cannot go back
    QHash<QString, QByteArray> Notes;
    QHash<QString, qint64> NotePins;
  };

  // A reply's entry of /v1/teams: nothing in it is believed but the wrap's bytes
  struct Listed
  {
    QString Uuid;
    qint64 RosterSeq = 0;
    QString RosterHash;
    qint64 HeadVersion = 0;
    QString HeadSha;
    qint64 MyKeyVersion = 0;
    QByteArray Wrap;
  };

  // A snapshot that passed every check
  struct Verified
  {
    qint64 Version = 0;
    QString Sha;
    qint64 KeyVersion = 0;
    QString Name;
    QByteArray Plain;
    QString SitesHash;
  };
  using FetchDone = std::function<void(bool Ok, const QString & Error, const Verified & Snapshot)>;

  Configuration * FConfig = nullptr;
  CloudSync * FSync = nullptr;
  TeamCrypto::KeyPair FKeys;
  TeamCrypto::SignKey FIdentity;
  QString FKeysUser;
  QString FAccount;
  QHash<QString, Team> FTeams;
  QHash<QString, TeamRoster::State> FRosters;
  QString FLastError;
  bool FSyncing = false;
  QList<Done> FWaiting;
  QList<Done> FNext;
  QHash<QString, QString> FResults;   // this sync's outcome per team
  QHash<QString, QString> FWarned;    // told already, per team
  QSet<QString> FGone;                // left or deleted while a sync ran
  QSet<QString> FForcePull;
  QHash<QString, Verified> FRestoring;   // goes up with the rotation that is pending
  bool FKeysBusy = false;
  QList<Done> FKeysWaiting;

  QHash<QString, State> FStates;
  QHash<QString, QString> FPinnedIdentities;
  bool FStatesLoaded = false;
  QByteArray FSavedStates;

  QString StateFileName() const;
  void LoadStates();
  void SaveStates();
  State & StateOf(const QString & Uuid);
  const State * FindState(const QString & Uuid) const;

  void StartSync();
  void SwitchAccount();
  void Finish(bool Ok, const QString & Error);
  // A sync started after the caller's change, answering for this team only
  void SyncFor(const QString & Uuid, const Done & Callback);
  void EnsureKeys(const Done & Callback);
  void FinishKeys(bool Ok, const QString & Error);
  bool AdoptKeys(const QByteArray & Wrapped, QString & Error);
  void UploadKeys();

  void FetchTeams(const Done & Callback);
  void VerifyListed(QList<Listed> Pending, QSet<QString> Seen, QHash<QString, Team> Teams, const Done & Callback);
  void CheckMissing(QStringList Pending, QHash<QString, Team> Teams, QStringList Errors, const Done & Callback);
  void BuildTeam(const Listed & Item, Team & T);
  void LoadRoster(const QString & Uuid, const std::function<void(bool Gone, const QString & Error)> & Callback);
  bool AcceptRoster(const QString & Uuid, const QJsonArray & Entries, QString & Why);
  // Not below what this computer verified; false with Why when it is
  static bool AboveFloor(const State & S, const TeamRoster::State & New, QString & Why);
  void RosterNotices(const QString & Uuid, const QList<TeamRoster::Entry> & Items, qint64 After);
  void Refused(Team & T, const QString & Why);
  QString FetchFailed(Team & T, const QString & Error);
  void Warn(const QString & Uuid, const QString & Message);

  void SyncTeams(QStringList Pending, QStringList Errors, const Done & Callback);
  void SyncTeam(const QString & Uuid, const Done & Callback, int Attempt = 0);
  void PullHead(const QString & Uuid, bool Revert, const Done & Callback, int Attempt);
  void Rotate(const QString & Uuid, const Done & Callback);
  // Team notes
  void FetchNotes(const QString & Uuid, const Done & Callback);
  bool AcceptNote(const QString & Uuid, const QString & Id, qint64 Version, const QString & Author, const QByteArray & Data);
  // A note accepted for the team's current key: its text and header
  bool ChannelNote(const QString & Uuid, const QString & Id, QString & Text, TeamCrypto::Note & Header) const;
  void PutNote(const QString & Uuid, const QString & Id, const QString & Text, const Done & Callback);
  void DropNote(const QString & Uuid, const QString & Id);
  // Owners and admins: the channel's notes into the team's sites here (before a
  // removal or a new key: then the next push signs them in)
  int FoldNotes(const QString & Uuid);
  Configuration & NoteConfig() const;
  QString EmailOf(const QString & Uuid, const QString & UserUuid) const;
  void Fetch(const QString & Uuid, qint64 Version, bool AsHead, const FetchDone & Callback);
  bool CheckSnapshot(const QString & Uuid, qint64 Version, const QByteArray & Data, bool AsHead, Verified & Result, QString & Why) const;
  void PinHead(const QString & Uuid, const Verified & Snapshot);
  void Push(const QString & Uuid, const Done & Callback, int Attempt);
  void Upload(const QString & Uuid, qint64 Base, const QByteArray & Data,
    const std::function<void(const QString & ObjectKey, const CloudClient::Reply & Failed)> & Callback);
  bool AfterPush(const QString & Uuid, qint64 Version, const QString & Sha, const QString & Name, const IniFile & Sites, QString & Error);
  bool Apply(const QString & Uuid, const Verified & Snapshot, QString & Error);
  void ApplyName(const QString & Uuid, const QString & Name);
  // Another folder came to be the same as the team's for the site list: the team moves
  void KeepFolderApart(const QString & Uuid);
  static void SetBase(State & S, const IniFile & Sites);
  void HoldConflict(const QString & Uuid, bool Empty, const Done & Callback);

  // Writes to the roster: re-reads it, signs on top of the real head, retries a lost race
  void RefreshRoster(const QString & Uuid, const std::function<void(const QString & Error)> & Then);
  void AppendEntry(const QString & Uuid, const QString & Type, const QJsonObject & Data, const Done & Callback, int Attempt = 0);
  // With the head the entry must name: a version this computer verified, never
  // what a server says the head is
  void AppendAtHead(const QString & Uuid, const QString & Type, QJsonObject Data, const Done & Callback);
  void SignDelete(const QString & Uuid, const Done & Callback, int Attempt);
  void Join(const TeamCrypto::Invite & Code, const Done & Callback, int Attempt);
  void SendInvite(const QString & Uuid, const QString & Role, int Days,
    const std::function<void(bool, const QString &, const QString &)> & Callback, int Attempt);

  IniFile LocalSites(const QString & Folder) const;
  QString UniqueFolder(const QString & Name, const QString & Uuid) const;
  QString UniqueFolderLike(const QString & Base, const QString & Uuid) const;
  QString EnsureFolder(const QString & Uuid);
  void MoveFolder(const QString & From, const QString & To);
  void RemoveFolderSites(const QString & Folder);
  // The team's folder goes; sites with local changes stay as personal sites
  int RemoveTeamLocally(const QString & Uuid);
  void ForgetTeam(const QString & Uuid, bool Ignore = false);
  void FinishMoves(State & S, const QHash<QString, QString> & Committed);
  // Saves the configuration after a change of team sites and tells the
  // open site lists (Login dialog) to reload them
  bool SaveSites();
};
