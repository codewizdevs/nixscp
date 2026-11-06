// Teams > Manage Teams...: NixSCP Teams (core/TeamSync). The teams on the
// left; the selected team's shared sites (share sites or folders, stop
// sharing), members (roles, removal) and pending invites on the right.
// ShareSites / StopSharing are also used by the Login dialog and the Teams
// menu of the main window.
#pragma once

#include <QDialog>
#include <QStringList>

#include <functional>

class QLabel;
class QListWidget;
class QMenu;
class QPushButton;
class QTabWidget;
class QTableWidget;
class QToolButton;
class QTreeWidget;

class TeamsDialog : public QDialog
{
  Q_OBJECT

public:
  explicit TeamsDialog(QWidget * Parent = nullptr, const QString & SelectUuid = QString());
  static void Execute(QWidget * Parent, const QString & SelectUuid = QString());

  // "Share with Team": the team (owners and admins), an optional subfolder
  // and whether the originals stay in the personal sites. Names: stored site
  // names. Returns true when shared.
  static bool ShareSites(QWidget * Parent, const QStringList & Names, const QString & Subfolder = QString(),
    const QString & TeamUuid = QString());
  // "Stop Sharing": confirms, then removes the sites (names relative to the
  // team's folder; "sub/" = a whole subfolder) from the team, keeping a
  // personal copy when chosen. Returns true when done.
  static bool StopSharing(QWidget * Parent, const QString & TeamUuid, const QStringList & Names);
  // The team of a stored site name and the name relative to its folder
  static bool TeamSite(const QString & SiteName, QString & TeamUuid, QString & Relative);
  // New team / join: the uuid of the new team (empty when cancelled or failed)
  static QString NewTeam(QWidget * Parent);
  static QString Join(QWidget * Parent);
  // The Teams menu of the main window; CurrentSite: the stored site of the
  // current tab (empty: none)
  static void SetupMenu(QMenu * Menu, QWidget * Window, const std::function<QString()> & CurrentSite);

private:
  QListWidget * FTeams;
  QLabel * FStatus;
  QLabel * FTitle;
  QLabel * FSubtitle;
  QTabWidget * FTabs;
  QWidget * FDetails;
  QLabel * FEmpty;
  QLabel * FBanner;
  QPushButton * FKeepMine;
  QPushButton * FUseTeam;
  QPushButton * FForget;
  // Shared sites
  QTreeWidget * FSites;
  QPushButton * FShareSites;
  QPushButton * FShareFolder;
  QPushButton * FUnshare;
  QLabel * FSitesHint;
  // Members
  QTableWidget * FMembers;
  QLabel * FFingerprint;
  QPushButton * FInvite;
  QPushButton * FMakeAdmin;
  QPushButton * FMakeMember;
  QPushButton * FRemoveMember;
  // Invites
  QTableWidget * FInvites;
  QPushButton * FRevoke;
  QLabel * FInvitesHint;
  // Team
  QToolButton * FTeamMenu;
  QString FLoadedMembers;

  void Reload();
  void ReloadSites();
  void LoadMembers(bool Force = false);
  void UpdateButtons();
  QString SelectedUuid() const;
  QStringList SelectedShares() const;
  // runs an operation with a busy cursor and reports its error
  void Run(const std::function<void(const std::function<void(bool, const QString &)> &)> & Operation);

  void Invite();
  void ShareSitesClicked();
  void ShareFolderClicked();
  void UnshareClicked();
  void SetRole(const QString & Role);
  void RemoveMember();
  void Rename();
  void RestoreFromHistory();
  void Forget();
  void Leave();
  void Delete();
};
