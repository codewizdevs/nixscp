// Port of WinSCP source/forms/Preferences.{cpp,dfm} (+ forms/EditorPreferences
// for the Add/Edit editor dialog, the master password dialog of
// windows/UserInterface.cpp and the Import/Export configuration actions of
// forms/Login.cpp).
// Pages: Environment (Interface, Window, Commander, Languages), Panels (File
// colors, Remote, Local), Editors (Internal editor), Transfer (Drag & Drop, Background,
// Endurance), Network, Security, Logging, Integration (Applications), Commands,
// Storage; search box.
// Also applies the preferences that belong to the whole application whenever
// they change (interface theme; UI language at startup) and handles the master
// password prompt.
// Not ported: Explorer (NixSCP has the Commander interface only).
// Removed (no Linux equivalent): Updates page, Windows registry storage, random seed
// file, PuTTY registry key, authentication agent choice (ssh-agent only), tips,
// "Get more" translations.
#pragma once

#include <QDialog>

class PreferencesDialog : public QDialog
{
  Q_OBJECT

public:
  explicit PreferencesDialog(QWidget * Parent = nullptr);
  ~PreferencesDialog() override;

  // Shows Preferences for Configuration::Instance(); on OK stores the options
  // and saves the configuration. InitialPage is a page caption, e.g. "Editors",
  // "Transfer", "Logging" (case insensitive); empty = first page.
  static bool Execute(QWidget * Parent, const QString & InitialPage = QString());

  void SelectPage(const QString & Caption);
  // Search box: lists the matching options on the Search page
  void Search(const QString & Text);
  void accept() override;

  // Applies the application-wide preferences (interface theme; UI language
  // once at startup). Connected to Configuration::Changed automatically when
  // the QApplication is created.
  static void ApplyConfiguration();
  // windows/UserInterface.cpp DoMasterPasswordDialog / DoChangeMasterPasswordDialog:
  // Current = ask for the current master password (Security::SetMasterPassword
  // is called with it); otherwise ask for a new one (returned in NewPassword)
  static bool DoMasterPasswordDialog(QWidget * Parent, bool Current, QString * NewPassword);
  // Login dialog: Tools > Export/Backup Configuration... and Import/Restore Configuration...
  static void ExportConfiguration(QWidget * Parent);
  static bool ImportConfiguration(QWidget * Parent);

private:
  struct Private;
  Private * d;

  void LoadConfiguration();
  void SaveConfiguration();
  bool SaveStorage();
  void UpdateControls();
  void UpdateEditorListView();
  void UpdateFileColorsView();
  bool EditEditor(int Index);
  void RegisterAsUrlHandler(bool Register);
  void CreateDesktopIcon();
  void UseMasterPasswordClicked();
  void ChangeMasterPassword(const QString & Message);
  void MasterPasswordChanged(const QString & Message, const QStringList & Errors);
  void LoadLanguages();
};
