// Port of WinSCP source/forms/Login.{cpp,dfm} (Login dialog / Site Manager),
// forms/ImportSessions.{cpp,dfm} (Import sites), forms/Cleanup.{cpp,dfm}
// (Clean Up) and the "Save session as site" dialog of forms/Custom.cpp
// (TSaveSessionDialog, DoSaveSession, SessionNameValidate).
#pragma once

#include "core/SessionData.h"
#include "core/SiteImport.h"

#include <QDialog>
#include <QHash>
#include <QIcon>
#include <functional>
#include <memory>

class QTreeWidget;
class QTreeWidgetItem;
class QComboBox;
class QLineEdit;
class QSpinBox;
class QPushButton;
class QToolButton;
class QPlainTextEdit;
class QGroupBox;
class QLabel;
class QCheckBox;
class QAction;
class QActionGroup;
class QMenu;
class QWidget;
class QListWidget;
class QStackedWidget;
class SessionTreeWidget;

class LoginDialog : public QDialog
{
  Q_OBJECT

public:
  LoginDialog(StoredSessionList & Sessions, QWidget * Parent = nullptr);
  ~LoginDialog() override;

  // TLoginDialog::Execute(DataList): call before exec(). A stored site of the
  // same name is selected, otherwise Data is loaded into "New Site" (session
  // being reconnected or that failed to connect).
  void SetInitialSession(const SessionData & Data);
  // ShowAgainPanel ("Show Login dialog on startup and when the last session
  // is closed"): WinSCP shows it when the dialog was opened automatically
  void SetShowAgainPanel(bool Show);

  // Sessions to open after Accepted: the site, or all sites of a folder or
  // workspace, in tab order (with environment variables expanded)
  QList<SessionData> SessionsToOpen() const { return FSessionsToOpen; }
  // First session to open (nullptr when none)
  std::shared_ptr<SessionData> SessionToOpen() const;
  // Workspace (or site folder) whose sessions are to be opened; empty for a site
  QString WorkspaceToOpen() const { return FWorkspaceToOpen; }

  // Tools > Import Sites (DoImportSessionsDialog); returns names of imported sites
  static bool ImportSites(StoredSessionList & Sessions, QWidget * Parent, QStringList * Imported = nullptr);
  // First run: offer to import sites of PuTTY, FileZilla and OpenSSH
  // (ImportSitesIfAny, IMPORT_SESSIONS2), once (AutoImportedFromPuttyOrFilezilla)
  static void ImportSitesIfAny(StoredSessionList & Sessions, QWidget * Parent);

  // Hooks for Login dialog commands implemented by other components; the
  // commands are only offered when the hook is set:
  // "Open in PuTTY" (Ctrl+P; terminal with ssh), "Generate Session URL/Code...",
  // "Transfer Settings Rule..."
  static std::function<void(const QList<SessionData> &, QWidget *)> OpenInPuttyHook;
  static std::function<void(const SessionData &, QWidget *)> GenerateUrlHook;
  static std::function<void(const SessionData &, QWidget *)> CopyParamRuleHook;

  void done(int Result) override;

protected:
  void keyPressEvent(QKeyEvent * Event) override;
  bool eventFilter(QObject * Object, QEvent * Event) override;
  void showEvent(QShowEvent * Event) override;

private:
  enum ItemType { itNewSite = 1, itSite, itFolder, itWorkspace };

  StoredSessionList & FSessions;
  QList<SessionData> FSessionsToOpen;
  QString FWorkspaceToOpen;
  SessionData FNewSiteData;
  // FSessionData: advanced settings of the edited site / New Site
  SessionData FEditingData;
  bool FEditing = false;
  QString FEditingName;                 // the site being edited
  bool FRenaming = false;
  int FNoUpdate = 0;
  bool FInitialized = false;
  bool FNewSiteKeepName = false;
  bool FForceNewSite = false;
  bool FUpdatePortWithProtocol = true;
  int FDefaultPort = SshPortNumber;
  int FSiteSearch;
  // TIncrementalSearchState
  bool FSearching = false;
  QString FSearchText;
  bool FSearchHaveNext = false;
  int FIncrementalSearching = 0;
  QString FRenamingOriginal;
  // PasteUrlAction: clipboard holds a session URL (updated on clipboard changes)
  bool FClipboardHasUrl = false;
  void UpdateClipboardUrl();

  SessionTreeWidget * SessionTree;
  QLabel * SitesIncrementalSearchLabel;
  QPushButton * ToolsMenuButton;
  QPushButton * ManageButton;
  QWidget * SitePanel;
  QGroupBox * BasicGroup;
  QWidget * ContentsPanel;
  QGroupBox * ContentsGroupBox;
  QLineEdit * ContentsNameEdit;
  QPlainTextEdit * ContentsMemo;
  QComboBox * TransferProtocolCombo;
  QLineEdit * TransferProtocolView;
  QLabel * EncryptionLabel;
  QComboBox * FtpsCombo;
  QComboBox * WebDavsCombo;
  QLineEdit * EncryptionView;
  QLineEdit * HostNameEdit;
  QSpinBox * PortNumberEdit;
  QLabel * UserNameLabel;
  QLineEdit * UserNameEdit;
  QLabel * PasswordLabel;
  QLineEdit * PasswordEdit;
  QCheckBox * AnonymousLoginCheck;
  QCheckBox * S3CredentialsEnvCheck;
  QComboBox * S3ProfileCombo;
  QToolButton * SaveButton;
  QPushButton * EditCancelButton;
  QPushButton * EditButton;
  QToolButton * AdvancedButton;
  QPushButton * AgentAccessButton;
  QPushButton * TeamButton;
  QGroupBox * NoteGroup;
  QPlainTextEdit * NoteMemo;
  QCheckBox * NoteShareCheck;
  QCheckBox * NoteTeamEditCheck;
  QCheckBox * NoteAgentReadCheck;
  QCheckBox * NoteAgentWriteCheck;
  QLabel * NoteInfo;
  QPushButton * NoteEditButton;
  QPushButton * NoteSaveButton;
  QPushButton * NoteCancelButton;
  // the note of a stored site is edited and saved on its own (not with the site)
  bool FNoteEditing = false;
  bool FNoteMayEdit = false;
  QString FNoteStatus;
  QString FNoteSite;
  void UpdateNotePanel(const SessionData & Data);
  void UpdateNoteInfo();
  void StartNoteEdit();
  bool SaveNote();
  void CancelNoteEdit();
  bool EnsureNoteSaved();
  void NoteAgentChecked(bool Write, bool Checked);
  void NoteShareChecked(bool Checked);
  void NoteTeamEditChecked(bool Checked);
  static QString NoteTip(const SessionData & Data);
  QPushButton * LoginButton;
  QPushButton * CloseButton;
  QWidget * ShowAgainPanel;
  QCheckBox * ShowAgainCheck;

  // ActionList
  QAction * EditSessionAction;
  QAction * SaveAsSessionAction;
  QAction * SaveSessionAction;
  QAction * DeleteSessionAction;
  QAction * ImportSessionsAction;
  QAction * LoginAction;
  QAction * AboutAction;
  QAction * CleanUpAction;
  QAction * ResetNewSessionAction;
  QAction * SetDefaultSessionAction;
  QAction * DesktopIconAction;
  QAction * RenameSessionAction;
  QAction * NewSessionFolderAction;
  QAction * NewNoteAction;
  QAction * RunPageantAction;
  QAction * RunPuttygenAction;
  QAction * ImportAction;
  QAction * ExportAction;
  QAction * PreferencesAction;
  QAction * EditCancelAction;
  QAction * SessionAdvancedAction;
  QAction * AgentAccessAction;
  // NixSCP Teams: share the selected site or folder, or stop sharing it
  QAction * ShareWithTeamAction;
  QAction * StopSharingAction;
  QAction * PreferencesLoggingAction;
  QAction * CloneToNewSiteAction;
  QAction * PasteUrlAction;
  QAction * SearchSiteNameStartOnlyAction;
  QAction * SearchSiteNameAction;
  QAction * SearchSiteAction;
  QAction * SessionRawAction;
  QAction * SearchSiteStartAction;
  QAction * PuttyAction;
  QAction * GenerateUrlAction2;
  QAction * CopyParamRuleAction;
  QMenu * ToolsPopupMenu;
  QMenu * SaveDropDownMenu;
  QAction * SaveAsSessionMenuItem;
  QMenu * SessionAdvancedPopupMenu;

  void CreateActions();
  void CreateControls();
  QMenu * CreateSearchOptionsMenu(QWidget * Parent);
  QMenu * GetSelectedNodePopupMenu();

  // tree
  void LoadSessions();
  QTreeWidgetItem * AddSessionPath(const QString & Path, bool CanCreate);
  QTreeWidgetItem * AddSession(const std::shared_ptr<SessionData> & Data);
  void SortTree();
  QIcon SessionIcon(const SessionData & Data);
public:
  // Site icon with the site color (GetSessionImageIndex), also for session tabs
  static QIcon SiteIcon(const SessionData & Data);
private:
  void UpdateNodeImage(QTreeWidgetItem * Node);
  void UpdateFolderNode(QTreeWidgetItem * Node);
  void UpdateFolderNodesStyle();
  QTreeWidgetItem * NewSiteNode() const;
  QTreeWidgetItem * Selected() const;
  void SetSelected(QTreeWidgetItem * Node);
  static ItemType NodeType(const QTreeWidgetItem * Node);
  bool IsSiteNode(const QTreeWidgetItem * Node) const { return (Node != nullptr) && (NodeType(Node) == itSite); }
  bool IsNewSiteNode(const QTreeWidgetItem * Node) const { return (Node != nullptr) && (NodeType(Node) == itNewSite); }
  bool IsFolderNode(const QTreeWidgetItem * Node) const { return (Node != nullptr) && (NodeType(Node) == itFolder); }
  bool IsWorkspaceNode(const QTreeWidgetItem * Node) const { return (Node != nullptr) && (NodeType(Node) == itWorkspace); }
  bool IsFolderOrWorkspaceNode(const QTreeWidgetItem * Node) const { return IsFolderNode(Node) || IsWorkspaceNode(Node); }
  QString SessionNodePath(const QTreeWidgetItem * Node) const;
  std::shared_ptr<SessionData> NodeSession(const QTreeWidgetItem * Node) const;
  std::shared_ptr<SessionData> SelectedSession() const;
  QTreeWidgetItem * FindSessionNode(const QString & Name) const;
  QTreeWidgetItem * SessionFolderNode(QTreeWidgetItem * Node) const;
  bool HasNodeAnySession(const QTreeWidgetItem * Node, bool NeedCanOpen = false) const;
  QString FolderOrWorkspaceContents(const QTreeWidgetItem * Node, const QString & Indent, const QString & CommonRoot) const;
  QList<QTreeWidgetItem *> AllNodes() const;
  void ReloadSessions(const QString & SelectSite);

  // state (Configuration\Interface: LastStoredSession, OpenedStoredSessionFolders;
  // Configuration\Interface\LoginDialog: WindowSize, SiteSearch)
  void SaveState();
  void LoadState();

  // session editing
  void LoadContents();
  void LoadSession(const SessionData & Data);
  void SaveSession(SessionData & Data);
  bool IsEditable() const;
  void UpdateControls();
  void UpdateActions();
  TFSProtocol GetFSProtocol(bool RequireScpFallbackDistinction) const;
  TFtps GetFtps() const;
  int DefaultPort() const;
  void SetFtpsIndex(int Index);
  void UpdatePortWithProtocol();
  // TLoginDialog::GetS3Profile / LoadS3Profiles / UpdateS3Credentials
  QString GetS3Profile() const;
  void LoadS3Profiles();
  void UpdateS3Credentials();
  void TransferProtocolComboChange();
  void PortNumberEditChange();
  bool EnsureNotEditing();
  void CancelEditing();
  void PersistNewSiteIfNeeded();
  void ParseHostName();
  bool ParseUrl(const QString & Url);
  bool CanOpen() const;
  bool IsSiteAndCanOpen() const;
  bool IsFolderOrWorkspaceAndCanOpen() const;
  bool IsCloneToNewSiteDefault() const;
  void ResetNewSiteData();
  void NewSite();
  void Default();
  void EditSession();
  void SaveConfiguration();
  void SaveDataList();

  // actions
  void LoginActionExecute();
  void EditSessionActionExecute();
  void SaveSessionActionExecute();
  void SaveAsSession(bool ForceDialog);
  void EditCancelActionExecute();
  void DeleteSessionActionExecute();
  void RenameSessionActionExecute();
  void SessionTreeEdited(QTreeWidgetItem * Node);
  void NewSessionFolderActionExecute();
  void NewNoteActionExecute();
  static QIcon NoteItemIcon();
  void CloneToNewSite();
  void ResetNewSessionActionExecute();
  void SetDefaultSessionActionExecute();
  void AgentAccessActionExecute();
  void ShareWithTeamActionExecute();
  void StopSharingActionExecute();
  // The team of the selected node (site or folder), with its name relative
  // to the team's folder ("sub/" for a folder); false when not in a team
  bool SelectedTeamItem(QString & TeamUuid, QString & Relative) const;
  void SessionAdvancedActionExecute(bool Raw);
  void ImportSessionsActionExecute();
  void ImportActionExecute();
  void ExportActionExecute();
  void CleanUpActionExecute();
  void DesktopIconActionExecute();
  void PasteUrlActionExecute();
  void PuttyActionExecute();
  // GetSessionData: the selected site, or New Site with the current values
  SessionData CurrentSessionData();
  void AboutActionExecute();
  void SessionTreeDropped(QTreeWidgetItem * Node, QTreeWidgetItem * DropTarget);
  bool SessionAllowDrop(QTreeWidgetItem * DropTarget) const;
  QTreeWidgetItem * NormalizeDropTarget(QTreeWidgetItem * DropTarget) const;
  bool CheckDuplicateFolder(QTreeWidgetItem * Parent, const QString & Text, QTreeWidgetItem * Node);
  bool CheckIsSessionFolder(QTreeWidgetItem * Node);
  bool SessionNameValidate(const QString & Text, const QString & OriginalName);
  std::shared_ptr<SessionData> DoSaveSession(SessionData & Data, const SessionData * OriginalSession, bool ForceDialog,
    const QStringList & AdditionalFolders);

  // incremental site search
  void ResetSitesIncrementalSearch();
  bool SitesIncrementalSearch(const QString & Text, bool SkipCurrent, bool Reverse, bool Expanding);
  QTreeWidgetItem * GetNextNode(QTreeWidgetItem * Node, bool Reverse) const;
  QTreeWidgetItem * SearchSite(const QString & Text, bool AllowExpanding, bool SkipCurrent, bool Reverse) const;
  void SetSiteSearch(int SiteSearch);
  void SearchSiteStartActionExecute();
  bool SessionTreeKeyPress(QKeyEvent * Event);
};

// TImportSessionsDialog
class ImportSessionsDialog : public QDialog
{
  Q_OBJECT

public:
  ImportSessionsDialog(StoredSessionList & Sessions, QWidget * Parent = nullptr);
  ~ImportSessionsDialog() override;

  // Reads all sources (also used by tests through SetSourceFiles)
  void Init();
  // Imports the checked sites (and host keys); returns the imported sites
  QList<std::shared_ptr<SessionData>> Import();
  int SourceIndex() const;
  void SetSourceIndex(int Index);
  void SetIniFileName(const QString & FileName);

private:
  StoredSessionList & FSessions;
  QList<ImportSessionList> FLists;
  QString FIniFileName;
  QComboBox * SourceComboBox;
  QPushButton * BrowseButton;
  QPushButton * PasteButton;
  QStackedWidget * ListStack;
  QListWidget * SessionListView;
  QLabel * ErrorLabel;
  QPushButton * CheckAllButton;
  QPushButton * OKButton;

  void LoadSessions();
  void SaveSelection();
  void UpdateControls();
  void BrowseButtonClick();
  void PasteButtonClick();
  void CheckAllButtonClick();
};
