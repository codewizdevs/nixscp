// Port of the workspace part of WinSCP source/core/SessionData.cpp
// (TStoredSessionList::GetWorkspaces, NewWorkspace, SaveWorkspaceData,
// GetFolderOrWorkspace, GetFolderOrWorkspaceList, ResolveWorkspaceData,
// IsWorkspace, IsFolder; TSessionData::IsInFolderOrWorkspace, HasStateData,
// CopyStateData) and of forms/CustomScpExplorer.cpp (DoSaveWorkspace,
// SaveWorkspace password logic, WorkspaceName).
//
// WinSCP stores a workspace as ordinary sites with IsWorkspace=1, one per tab,
// named "<workspace>/<tab index as 4 hex digits>" (IntToHex(Index, 4)):
//
//   [Sessions\My%20Workspace/0000]     tab with a stored site:
//   IsWorkspace=1                        Link = name of the stored site, plus
//   Link=Work/Server                     the tab state (RemoteDirectory,
//   RemoteDirectory=/var/www             LocalDirectory, OtherLocalDirectory,
//   LocalDirectory=/home/me/www          SynchronizeBrowsing, Color)
//
//   [Sessions\My%20Workspace/0001]     ad-hoc tab (not a stored site):
//   HostName=example.com                 complete site settings, NameOverride =
//   UserName=me                          session name (if it had one); the
//   IsWorkspace=1                        password only when "Save passwords"
//   ...
//
// Saving a workspace replaces all sites under "<workspace>/" (NewWorkspace).
#pragma once

#include "core/SessionData.h"

#include <QList>
#include <QString>
#include <QStringList>

class Configuration;

namespace Workspaces
{
  // NEW_WORKSPACE: default name offered by "Save Workspace"
  QString NewWorkspaceName();

  // TStoredSessionList::GetWorkspaces: names of all workspaces (folder part of
  // the IsWorkspace sites), sorted, case insensitively unique
  QStringList WorkspaceNames(const StoredSessionList & Sessions);
  // TStoredSessionList::IsWorkspace / IsFolder / HasAnyWorkspace
  bool IsWorkspace(const StoredSessionList & Sessions, const QString & Name);
  bool IsFolder(const StoredSessionList & Sessions, const QString & Name);
  bool HasAnyWorkspace(const StoredSessionList & Sessions);
  // TSessionData::IsInFolderOrWorkspace: Data.Name starts with "Folder/" (case insensitive)
  bool IsInFolderOrWorkspace(const SessionData & Data, const QString & Folder);

  // TStoredSessionList::GetFolderOrWorkspace: the sessions to open, one per tab,
  // in tab order. Links are resolved to (copies of) the stored sites with the
  // saved tab state applied; links to sites that no longer exist and sites
  // that cannot be opened (no host name) are skipped. Ad-hoc sessions keep
  // IsWorkspace=true and get their NameOverride as Name (empty if none), so
  // saving the workspace again keeps them ad-hoc - clear IsWorkspace before
  // saving such a session as a stored site (as CloneCurrentSessionData does).
  // Works for site folders too ("open all sites in the folder").
  QList<SessionData> FolderOrWorkspace(const StoredSessionList & Sessions, const QString & Name);
  QList<SessionData> LoadWorkspace(const StoredSessionList & Sessions, const QString & Name);
  // TStoredSessionList::GetFolderOrWorkspaceList: session names, for the Login
  // dialog contents panel / tooltip
  QStringList FolderOrWorkspaceList(const StoredSessionList & Sessions, const QString & Name);

  // Workspace entry name: "<Workspace>/<IntToHex(Index, 4)>"
  QString EntryName(const QString & Workspace, int Index);

  // TStoredSessionList::FindSame: the stored site a tab was opened from
  // (nullptr for unnamed sessions, workspace sessions and deleted sites)
  std::shared_ptr<SessionData> FindSame(const StoredSessionList & Sessions, const SessionData & Data);
  // TStoredSessionList::ResolveWorkspaceData: follows Link (nullptr = dangling)
  std::shared_ptr<SessionData> ResolveWorkspaceData(const StoredSessionList & Sessions, std::shared_ptr<SessionData> Data);

  // TStoredSessionList::SaveWorkspaceData + NewWorkspace (+ DoSaveWorkspace
  // password clearing): replaces workspace Name with the given tabs. Each tab
  // is the session data of the tab with its current directories in
  // RemoteDirectory/LocalDirectory (and the session password in Password, if
  // it should be saved with an ad-hoc session). Tabs of stored sites (same
  // Name, see FindSame) are saved as links; others completely, without
  // passwords unless SavePasswords. Only modifies the list: call
  // Configuration::Save() (or StoredSessionList::Save) afterwards.
  void SaveWorkspace(StoredSessionList & Sessions, const QString & Name, const QList<SessionData> & Tabs, bool SavePasswords);
  // Removes the workspace (all sites under "Name/", as the Login dialog does);
  // returns the number of removed sessions
  int DeleteWorkspace(StoredSessionList & Sessions, const QString & Name);

  // TSessionData::HasStateData / CopyStateData
  bool HasStateData(const SessionData & Data);
  void CopyStateData(SessionData & Target, const SessionData & Source);
  // TSessionData::HasAnySessionPassword / ClearSessionPasswords
  bool HasAnySessionPassword(const SessionData & Data);
  void ClearSessionPasswords(SessionData & Data);

  // TCustomScpExplorerForm::SaveWorkspace: state of the "Save passwords"
  // check box of the Save workspace dialog for the given tabs
  struct PasswordOptions
  {
    bool CanSavePasswords = false;   // any ad-hoc session has a password (else the box is disabled)
    bool SavePasswords = false;      // initial state of the box
    bool NotRecommended = false;     // caption "Save passwords (not recommended)"
  };
  PasswordOptions SavePasswordOptions(const StoredSessionList & Sessions, const QList<SessionData> & Tabs);

  // TWinConfiguration::LastWorkspace (not persisted, as in WinSCP): the last
  // opened/saved workspace of this process
  QString LastWorkspace();
  void SetLastWorkspace(const QString & Name);
  // TCustomScpExplorerForm::WorkspaceName: LastWorkspace, else AutoWorkspace, else "My Workspace"
  QString WorkspaceName(const Configuration & Config);

  // TSessionData::ValidateName + TSaveWorkspaceDialog::DoValidate:
  // empty string when Name can be used for a workspace, else the error message
  QString ValidateWorkspaceName(const StoredSessionList & Sessions, const QString & Name);
}
