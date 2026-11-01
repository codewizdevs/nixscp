// Port of WinSCP source/forms/Custom.cpp TSaveWorkspaceDialog
// (DoSaveWorkspaceDialog, "Save workspace as") and of
// forms/CustomScpExplorer.cpp TCustomScpExplorerForm::SaveWorkspace /
// DoSaveWorkspace (Tabs > Save Workspace...).
// Workspaces themselves are core/Workspaces.
#pragma once

#include "core/SessionData.h"

#include <QDialog>
#include <QList>
#include <QString>

class QCheckBox;
class QComboBox;
class QPushButton;

class SaveWorkspaceDialog : public QDialog
{
  Q_OBJECT

public:
  // DoSaveWorkspaceDialog. SavePasswords == nullptr: the "Save passwords"
  // box is disabled (no ad-hoc session has a password).
  // NotRecommendedSavingPasswords: caption "Save passwords (not recommended)".
  // Validates the name (no "/", not a site folder) before accepting.
  static bool Execute(QWidget * Parent, const StoredSessionList & Sessions, QString & WorkspaceName,
    bool * SavePasswords, bool NotRecommendedSavingPasswords, bool & CreateShortcut, bool & EnableAutoSave);

  // TCustomScpExplorerForm::SaveWorkspace for Configuration::Instance():
  // Tabs are the session data of all open tabs (in tab order) with their
  // current RemoteDirectory/LocalDirectory and the session password in
  // Password (see Workspaces::SaveWorkspace). Shows the dialog (EnableAutoSave
  // is the initial state of "Enable automatic saving"), saves the workspace,
  // remembers it as Workspaces::LastWorkspace, creates the desktop shortcut,
  // updates AutoSaveWorkspace/AutoWorkspace/AutoSaveWorkspacePasswords and
  // saves the configuration. Returns false when cancelled.
  static bool SaveWorkspace(QWidget * Parent, const QList<SessionData> & Tabs, bool EnableAutoSave = false,
    QString * SavedName = nullptr);

  // CreateDesktopSessionShortCut(sscWorkspace): <Desktop>/<name>.desktop
  // running "nixscp <workspace name>"; shows an error box on failure
  static bool CreateDesktopShortcut(QWidget * Parent, const QString & WorkspaceName);

  ~SaveWorkspaceDialog() override;

  void accept() override;

private:
  SaveWorkspaceDialog(QWidget * Parent, const StoredSessionList & Sessions, bool CanSavePasswords,
    bool NotRecommendedSavingPasswords);

  const StoredSessionList & FSessions;
  QComboBox * WorkspaceNameCombo;
  QCheckBox * SavePasswordsCheck;
  QCheckBox * CreateShortcutCheck;
  QCheckBox * EnableAutoSaveCheck;
  QPushButton * OkButton;

  void UpdateControls();
};
