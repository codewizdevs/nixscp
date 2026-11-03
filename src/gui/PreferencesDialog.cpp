// Port of WinSCP source/forms/Preferences.cpp + Preferences.dfm,
// source/forms/EditorPreferences.cpp + EditorPreferences.dfm, the master password
// dialog of windows/UserInterface.cpp, locale handling of windows/GUIConfiguration.cpp
#include "gui/PreferencesDialog.h"
#include "gui/UpdateCheck.h"
#include "gui/GuiCommandLine.h"
#include "gui/CloudSyncUi.h"
#include "gui/HelpSystem.h"
#include "gui/CustomCommandsPage.h"
#include "gui/CopyParamsDialog.h"
#include "gui/EditMaskDialog.h"
#include "gui/EditorWindow.h"
#include "gui/FileListModel.h"
#include "gui/Translator.h"
#include "core/ConfigVault.h"
#include "core/Configuration.h"
#include "core/CopyParamList.h"
#include "core/FileMasks.h"
#include "core/IniStorage.h"
#include "core/Security.h"
#include "core/SessionLog.h"
#include "core/Workspaces.h"

#include <QHash>
#include <QSystemTrayIcon>
#include <QAction>
#include <QActionEvent>
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QCursor>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLibraryInfo>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPalette>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleHints>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QToolTip>
#include <QTranslator>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>

static const QString UrlHandlerDesktopFile = QStringLiteral("nixscp-url-handler.desktop");
static const QStringList UrlSchemes = {
  QStringLiteral("sftp"), QStringLiteral("scp"), QStringLiteral("ftp"), QStringLiteral("ftps"),
  QStringLiteral("ftpes"), QStringLiteral("dav"), QStringLiteral("davs"), QStringLiteral("s3") };

//---------------------------------------------------------------------------
// SizeToStr (core/Common.cpp); TryStrToSize is in core/FileMasks.h
// HelpKeyword of the TTabSheets of forms/Preferences.dfm (LanguagesSheet has
// none: the form's ui_preferences)
static QString PreferencesPageHelpKeyword(const QString & Caption)
{
  static const QHash<QString, QString> Keywords = {
    { QStringLiteral("Environment"), QStringLiteral("ui_pref_environment") },
    { QStringLiteral("Interface"), QStringLiteral("ui_pref_interface") },
    { QStringLiteral("Window"), QStringLiteral("ui_pref_window") },
    { QStringLiteral("Commander"), QStringLiteral("ui_pref_commander") },
    { QStringLiteral("Explorer"), QStringLiteral("ui_pref_explorer") },
    { QStringLiteral("Languages"), QStringLiteral("ui_preferences") },
    { QStringLiteral("Panels"), QStringLiteral("ui_pref_panels") },
    { QStringLiteral("File colors"), QStringLiteral("ui_pref_file_colors") },
    { QStringLiteral("Remote"), QStringLiteral("ui_pref_panels_remote") },
    { QStringLiteral("Local"), QStringLiteral("ui_pref_panels_local") },
    { QStringLiteral("Editors"), QStringLiteral("ui_pref_editor") },
    { QStringLiteral("Internal editor"), QStringLiteral("ui_pref_editor_internal") },
    { QStringLiteral("Transfer"), QStringLiteral("ui_pref_transfer") },
    { QStringLiteral("Drag & Drop"), QStringLiteral("ui_pref_dragdrop") },
    { QStringLiteral("Background"), QStringLiteral("ui_pref_background") },
    { QStringLiteral("Endurance"), QStringLiteral("ui_pref_resume") },
    { QStringLiteral("Network"), QStringLiteral("ui_pref_network") },
    { QStringLiteral("Security"), QStringLiteral("ui_pref_security") },
    { QStringLiteral("Logging"), QStringLiteral("ui_pref_logging") },
    { QStringLiteral("Integration"), QStringLiteral("ui_pref_integration") },
    { QStringLiteral("Applications"), QStringLiteral("ui_pref_integration_app") },
    { QStringLiteral("Commands"), QStringLiteral("ui_pref_commands") },
    { QStringLiteral("Storage"), QStringLiteral("ui_pref_storage") },
    // NixSCP's own new version check (not WinSCP's updater): the download page
    { QStringLiteral("Updates"), QStringLiteral("nixscp:download") },
  };
  return Keywords.value(Caption, QStringLiteral("ui_preferences"));
}

static QString SizeToStr(qint64 Size)
{
  if ((Size >= 1024LL * 1024 * 1024) && ((Size % (1024LL * 1024 * 1024)) == 0))
  {
    return QString::number(Size / (1024LL * 1024 * 1024)) + QLatin1Char('G');
  }
  if ((Size >= 1024 * 1024) && ((Size % (1024 * 1024)) == 0))
  {
    return QString::number(Size / (1024 * 1024)) + QLatin1Char('M');
  }
  if ((Size >= 1024) && ((Size % 1024) == 0))
  {
    return QString::number(Size / 1024) + QLatin1Char('K');
  }
  return QString::number(Size);
}

static QLabel * Buddy(const QString & Caption, QWidget * Control)
{
  auto * Label = new QLabel(Caption);
  Label->setBuddy(Control);
  return Label;
}

static QHBoxLayout * Indent(QWidget * W)
{
  auto * Row = new QHBoxLayout();
  Row->addSpacing(20);
  Row->addWidget(W);
  return Row;
}

static QSpinBox * SpinBox(int Min, int Max)
{
  auto * Edit = new QSpinBox();
  Edit->setRange(Min, Max);
  return Edit;
}

//---------------------------------------------------------------------------
// forms/EditorPreferences.cpp
class EditorPreferencesDialog : public QDialog
{
public:
  EditorPreferencesDialog(EditorPreferences & Editor, bool Add, QWidget * Parent) :
    QDialog(Parent),
    FEditor(Editor)
  {
    setWindowTitle(Add ? QStringLiteral("Add editor") : QStringLiteral("Edit editor"));
    auto * Layout = new QVBoxLayout(this);

    auto * EditorGroup = new QGroupBox(QStringLiteral("Editor"));
    auto * EG = new QGridLayout(EditorGroup);
    FInternalButton = new QRadioButton(QStringLiteral("&Internal editor"));
    FExternalButton = new QRadioButton(QStringLiteral("&External editor:"));
    FExternalEdit = new QLineEdit();
    auto * BrowseButton = new QPushButton(QStringLiteral("B&rowse..."));
    FOpenButton = new QRadioButton(QStringLiteral("&Associated application"));
    auto * DefaultButton = new QPushButton(QStringLiteral("Use default system editor"));
    EG->addWidget(FInternalButton, 0, 0, 1, 2);
    EG->addWidget(FExternalButton, 1, 0, 1, 2);
    EG->addWidget(FExternalEdit, 2, 0);
    EG->addWidget(BrowseButton, 2, 1);
    EG->addWidget(DefaultButton, 3, 1);
    EG->addWidget(FOpenButton, 4, 0, 1, 2);
    EG->setColumnStretch(0, 1);
    Layout->addWidget(EditorGroup);

    FExternalGroup = new QGroupBox(QStringLiteral("External editor options (affects editing remote files only)"));
    auto * XG = new QVBoxLayout(FExternalGroup);
    FTextCheck = new QCheckBox(QStringLiteral("Force &text transfer mode for files edited in external editor"));
    FSDICheck = new QCheckBox(QStringLiteral("E&xternal editor opens each file in separate window (process)"));
    XG->addWidget(FTextCheck);
    XG->addWidget(FSDICheck);
    Layout->addWidget(FExternalGroup);

    auto * MaskGroup = new QGroupBox(QStringLiteral("Editor autoselection"));
    auto * MG = new QVBoxLayout(MaskGroup);
    FMaskEdit = new QLineEdit();
    MG->addWidget(Buddy(QStringLiteral("Use this editor for &following files:"), FMaskEdit));
    MG->addWidget(FMaskEdit);
    Layout->addWidget(MaskGroup);

    auto * Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(Buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    HelpSystem::AddHelpButton(Buttons, this, QStringLiteral("ui_editor_preferences"));
    Layout->addWidget(Buttons);

    connect(BrowseButton, &QPushButton::clicked, this, [this]()
    {
      QString File = QFileDialog::getOpenFileName(this, QStringLiteral("Select editor"), QStringLiteral("/usr/bin"));
      if (!File.isEmpty())
      {
        FExternalEdit->setText(File.contains(QLatin1Char(' ')) ? (QLatin1Char('"') + File + QLatin1Char('"')) : File);
        FExternalButton->setChecked(true);
      }
    });
    connect(DefaultButton, &QPushButton::clicked, this, [this]()
    {
      // Linux counterpart of Notepad: $VISUAL/$EDITOR or the desktop text editor
      QString Editor = qEnvironmentVariable("VISUAL", qEnvironmentVariable("EDITOR"));
      if (Editor.isEmpty() || Editor.contains(QStringLiteral("vi")) || Editor.contains(QStringLiteral("nano")))
      {
        Editor = QStringLiteral("gnome-text-editor");
        for (const QString & Candidate : {QStringLiteral("gnome-text-editor"), QStringLiteral("gedit"), QStringLiteral("kate"),
               QStringLiteral("kwrite"), QStringLiteral("mousepad"), QStringLiteral("xed"), QStringLiteral("pluma")})
        {
          if (!QStandardPaths::findExecutable(Candidate).isEmpty())
          {
            Editor = Candidate;
            break;
          }
        }
      }
      FExternalEdit->setText(Editor + QStringLiteral(" !.!"));
      FExternalButton->setChecked(true);
    });
    for (QRadioButton * B : {FInternalButton, FExternalButton, FOpenButton})
    {
      connect(B, &QRadioButton::toggled, this, [this]() { UpdateControls(); });
    }
    connect(FExternalEdit, &QLineEdit::textChanged, this, [this]() { UpdateControls(); });

    FInternalButton->setChecked(Editor.Editor == edInternal);
    FExternalButton->setChecked(Editor.Editor == edExternal);
    FOpenButton->setChecked(Editor.Editor == edOpen);
    FExternalEdit->setText(Editor.ExternalEditor);
    FTextCheck->setChecked(Editor.ExternalEditorText);
    FSDICheck->setChecked(Editor.SDIExternalEditor);
    FMaskEdit->setText(Editor.FileMask);
    FOkButton = Buttons->button(QDialogButtonBox::Ok);
    UpdateControls();
  }

  void accept() override
  {
    // ValidateMaskEdit
    try
    {
      FileMasks Masks(FMaskEdit->text().trimmed());
    }
    catch (const FileMasksException & E)
    {
      QMessageBox::critical(this, windowTitle(), E.Message());
      FMaskEdit->setFocus();
      FMaskEdit->setSelection(E.ErrorStart, E.ErrorLen);
      return;
    }
    FEditor.Editor = FInternalButton->isChecked() ? edInternal : (FExternalButton->isChecked() ? edExternal : edOpen);
    FEditor.ExternalEditor = FExternalEdit->text().trimmed();
    FEditor.ExternalEditorText = FTextCheck->isChecked();
    FEditor.SDIExternalEditor = FSDICheck->isChecked();
    FEditor.FileMask = FMaskEdit->text().trimmed().isEmpty() ? QStringLiteral("*.*") : FMaskEdit->text().trimmed();
    QDialog::accept();
  }

private:
  void UpdateControls()
  {
    // Radio buttons are initialized (and emit toggled) before the OK button is known
    if (FOkButton == nullptr)
    {
      return;
    }
    FExternalEdit->setEnabled(FExternalButton->isChecked());
    FExternalGroup->setEnabled(FExternalButton->isChecked());
    FOkButton->setEnabled(!FExternalButton->isChecked() || !FExternalEdit->text().trimmed().isEmpty());
  }

  EditorPreferences & FEditor;
  QRadioButton * FInternalButton;
  QRadioButton * FExternalButton;
  QRadioButton * FOpenButton;
  QLineEdit * FExternalEdit;
  QGroupBox * FExternalGroup;
  QCheckBox * FTextCheck;
  QCheckBox * FSDICheck;
  QLineEdit * FMaskEdit;
  QPushButton * FOkButton = nullptr;
};

//---------------------------------------------------------------------------
// Interface theme (windows/VCLCommon.cpp dark mode; Linux: QStyleHints color
// scheme, which follows the desktop / XDG portal setting when Automatic)
static QPalette DarkPalette()
{
  QPalette Palette;
  QColor Window(45, 45, 45);
  QColor Base(30, 30, 30);
  QColor Text(230, 230, 230);
  QColor Disabled(127, 127, 127);
  QColor Highlight(42, 130, 218);
  Palette.setColor(QPalette::Window, Window);
  Palette.setColor(QPalette::WindowText, Text);
  Palette.setColor(QPalette::Base, Base);
  Palette.setColor(QPalette::AlternateBase, Window);
  Palette.setColor(QPalette::ToolTipBase, Base);
  Palette.setColor(QPalette::ToolTipText, Text);
  Palette.setColor(QPalette::PlaceholderText, Disabled);
  Palette.setColor(QPalette::Text, Text);
  Palette.setColor(QPalette::Button, Window);
  Palette.setColor(QPalette::ButtonText, Text);
  Palette.setColor(QPalette::BrightText, Qt::red);
  Palette.setColor(QPalette::Light, QColor(70, 70, 70));
  Palette.setColor(QPalette::Midlight, QColor(60, 60, 60));
  Palette.setColor(QPalette::Mid, QColor(40, 40, 40));
  Palette.setColor(QPalette::Dark, QColor(20, 20, 20));
  Palette.setColor(QPalette::Shadow, Qt::black);
  Palette.setColor(QPalette::Link, Highlight.lighter(130));
  Palette.setColor(QPalette::Highlight, Highlight);
  Palette.setColor(QPalette::HighlightedText, Qt::white);
  for (QPalette::ColorRole Role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText, QPalette::HighlightedText})
  {
    Palette.setColor(QPalette::Disabled, Role, Disabled);
  }
  return Palette;
}

static void ApplyTheme(TAutoSwitch DarkTheme)
{
  static int Applied = -1;
  static bool PaletteOverridden = false;
  static QPalette OriginalPalette;
  if (Applied == static_cast<int>(DarkTheme))
  {
    return;
  }
  Applied = static_cast<int>(DarkTheme);
  if (PaletteOverridden)
  {
    QApplication::setPalette(OriginalPalette);
    PaletteOverridden = false;
  }
  QStyleHints * Hints = QGuiApplication::styleHints();
  if (DarkTheme == asAuto)
  {
    Hints->unsetColorScheme();
    return;
  }
  Qt::ColorScheme Scheme = (DarkTheme == asOn) ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light;
  Hints->setColorScheme(Scheme);
  if (Hints->colorScheme() != Scheme)
  {
    // the platform theme cannot switch the scheme: own palette
    OriginalPalette = QApplication::palette();
    QApplication::setPalette((Scheme == Qt::ColorScheme::Dark) ? DarkPalette() : QApplication::style()->standardPalette());
    PaletteOverridden = true;
  }
}

void PreferencesDialog::ApplyConfiguration()
{
  Configuration & C = Configuration::Instance();
  ApplyTheme(C.DarkTheme);
  // TGUIConfiguration::SetLocaleSafe; NixSCP switches the language live
  Translator::SetLanguage(static_cast<unsigned int>(C.Locale));
}

//---------------------------------------------------------------------------
// windows/UserInterface.cpp: TMasterPasswordDialog
class MasterPasswordDialog : public QDialog
{
public:
  // Current: enter the current master password only; otherwise set a new one
  MasterPasswordDialog(bool Current, QWidget * Parent) :
    QDialog(Parent)
  {
    setWindowTitle(QStringLiteral("Master password"));
    auto * Layout = new QVBoxLayout(this);
    auto CreatePasswordEdit = [Layout](const QString & Caption)
    {
      auto * Edit = new QLineEdit();
      Edit->setEchoMode(QLineEdit::Password);
      Edit->setMaxLength(Security::PasswordMaxLength());
      Layout->addWidget(Buddy(Caption, Edit));
      Layout->addWidget(Edit);
      return Edit;
    };
    FCurrentEdit = CreatePasswordEdit(QStringLiteral("&Current master password:"));
    FCurrentEdit->setEnabled(Current || Security::UseMasterPassword());
    if (!Current)
    {
      FNewEdit = CreatePasswordEdit(QStringLiteral("&New master password:"));
      FConfirmEdit = CreatePasswordEdit(QStringLiteral("&Re-enter master password:"));
      if (!Security::UseMasterPassword())
      {
        FNewEdit->setFocus();
      }
    }
    auto * Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(Buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    // HELP_MASTER_PASSWORD_CURRENT / HELP_MASTER_PASSWORD_CHANGE
    HelpSystem::AddHelpButton(Buttons, this,
      Current ? QStringLiteral("ui_master_password") : QStringLiteral("ui_master_password_change"));
    Layout->addWidget(Buttons);
    FOkButton = Buttons->button(QDialogButtonBox::Ok);
    for (QLineEdit * Edit : findChildren<QLineEdit *>())
    {
      connect(Edit, &QLineEdit::textChanged, this, [this]() { DoChange(); });
    }
    setMinimumWidth(360);
    DoChange();
  }

  QString CurrentPassword() const { return FCurrentEdit->isEnabled() ? FCurrentEdit->text() : QString(); }
  QString NewPassword() const { return (FNewEdit != nullptr) ? FNewEdit->text() : QString(); }

  void accept() override
  {
    // TMasterPasswordDialog::DoValidate
    if (Security::UseMasterPassword() && !Security::ValidateMasterPassword(FCurrentEdit->text()))
    {
      QMessageBox::critical(this, windowTitle(), QStringLiteral("You did not enter the correct current master password."));
      FCurrentEdit->setFocus();
      FCurrentEdit->selectAll();
      return;
    }
    if (FNewEdit != nullptr)
    {
      if (FNewEdit->text() != FConfirmEdit->text())
      {
        QMessageBox::critical(this, windowTitle(), QStringLiteral("New and re-entered master passwords are not the same."));
        FConfirmEdit->setFocus();
        FConfirmEdit->selectAll();
        return;
      }
      if (Security::IsValidPassword(FNewEdit->text()) <= 0)
      {
        QMessageBox Box(QMessageBox::Warning, windowTitle(),
          QStringLiteral("Your password is too simple and may not provide enough security against dictionary or brute force attacks.\n"
                         "Are you sure you want to use it?"),
          QMessageBox::Ok | QMessageBox::Cancel, this);
        Box.setInformativeText(
          QStringLiteral("Note: good passwords have at least six characters and contain both lowercase and uppercase letters, "
                         "numbers and special characters, such as delimiters, symbols, letters with accent, etc."));
        if (Box.exec() == QMessageBox::Cancel)
        {
          FNewEdit->setFocus();
          FNewEdit->selectAll();
          return;
        }
      }
    }
    QDialog::accept();
  }

private:
  void DoChange()
  {
    // TMasterPasswordDialog::DoChange
    bool CanSubmit =
      (!Security::UseMasterPassword() || (Security::IsValidPassword(FCurrentEdit->text()) >= 0)) &&
      ((FNewEdit == nullptr) || (Security::IsValidPassword(FNewEdit->text()) >= 0)) &&
      ((FConfirmEdit == nullptr) || (Security::IsValidPassword(FConfirmEdit->text()) >= 0));
    if (!FCurrentEdit->isEnabled() && (FNewEdit == nullptr))
    {
      CanSubmit = !FCurrentEdit->text().isEmpty();
    }
    FOkButton->setEnabled(CanSubmit);
  }

  QLineEdit * FCurrentEdit;
  QLineEdit * FNewEdit = nullptr;
  QLineEdit * FConfirmEdit = nullptr;
  QPushButton * FOkButton;
};

bool PreferencesDialog::DoMasterPasswordDialog(QWidget * Parent, bool Current, QString * NewPassword)
{
  // windows/UserInterface.cpp DoMasterPasswordDialog
  MasterPasswordDialog Dialog(Current, Parent);
  if (Dialog.exec() != QDialog::Accepted)
  {
    return false;
  }
  QString CurrentPassword = Dialog.CurrentPassword();
  if ((Current || Security::UseMasterPassword()) && !CurrentPassword.isEmpty())
  {
    Security::SetMasterPassword(CurrentPassword);
  }
  if (NewPassword != nullptr)
  {
    *NewPassword = Dialog.NewPassword();
  }
  return true;
}

// TTerminalManager::MasterPasswordPrompt
static bool MasterPasswordPrompt()
{
  if (QThread::currentThread() != QCoreApplication::instance()->thread())
  {
    return false;
  }
  return PreferencesDialog::DoMasterPasswordDialog(QApplication::activeWindow(), true, nullptr);
}

//---------------------------------------------------------------------------
// Tools > Export/Backup Configuration, Import/Restore Configuration (forms/Login.cpp)
void PreferencesDialog::ExportConfiguration(QWidget * Parent)
{
  Configuration & C = Configuration::Instance();
  QString FileName = QFileDialog::getSaveFileName(Parent, QStringLiteral("Select file to export configuration to"),
    C.DefaultIniFileExportPath(), QStringLiteral("INI files (*.ini);;All files (*)"));
  if (FileName.isEmpty())
  {
    return;
  }
  QString Error;
  if (!CloudSyncUi::ExportConfiguration(Parent, FileName, &Error))
  {
    if (!Error.isEmpty())  // empty: cancelled
    {
      QMessageBox::critical(Parent, QStringLiteral("Error"), Error);
    }
  }
}

bool PreferencesDialog::ImportConfiguration(QWidget * Parent)
{
  // IMPORT_CONFIGURATION2; the configuration is reloaded in place (no restart needed)
  QMessageBox Box(QMessageBox::Warning, QStringLiteral("Warning"), QStringLiteral("Restore configuration from a backup?"),
    QMessageBox::Ok | QMessageBox::Cancel, Parent);
  Box.setInformativeText(QStringLiteral("Importing configuration will overwrite all your preferences and sites. "
    "Consider saving your work and creating backup of your current configuration."));
  if (Box.exec() != QMessageBox::Ok)
  {
    return false;
  }
  Configuration & C = Configuration::Instance();
  QString FileName = QFileDialog::getOpenFileName(Parent, QStringLiteral("Select file to import configuration from"),
    C.DefaultIniFileExportPath(), QStringLiteral("INI files (*.ini);;All files (*)"));
  if (FileName.isEmpty())
  {
    return false;
  }
  QString Error;
  if (!CloudSyncUi::ImportConfiguration(Parent, FileName, &Error))
  {
    if (!Error.isEmpty())  // empty: cancelled
    {
      QMessageBox::critical(Parent, QStringLiteral("Error"), Error);
    }
    return false;
  }
  return true;
}

//---------------------------------------------------------------------------
// Hooks into the GUI application, installed when QApplication is created:
// preferences are applied whenever they change (also right after Configuration::Load)
static void PreferencesStartup()
{
  if (qobject_cast<QApplication *>(QCoreApplication::instance()) == nullptr)
  {
    return;
  }
  Security::SetMasterPasswordPrompt(MasterPasswordPrompt);
  QObject::connect(&Configuration::Instance(), &Configuration::Changed, QCoreApplication::instance(),
    []() { PreferencesDialog::ApplyConfiguration(); });

  // Testing aid (like NIXSCP_SCREENSHOT in main.cpp): NIXSCP_PREFERENCES_SCREENSHOT=<file.png>
  // shows Preferences (on page NIXSCP_PREFERENCES_PAGE, searching NIXSCP_PREFERENCES_SEARCH)
  // and saves its picture; NIXSCP_PREFERENCES_QUIT quits afterwards
  QString Screenshot = qEnvironmentVariable("NIXSCP_PREFERENCES_SCREENSHOT");
  if (!Screenshot.isEmpty())
  {
    // the application is still being constructed: start once the event loop runs
    QMetaObject::invokeMethod(QCoreApplication::instance(), [Screenshot]()
    {
      auto * Dialog = new PreferencesDialog();
      Dialog->setAttribute(Qt::WA_DeleteOnClose);
      Dialog->SelectPage(qEnvironmentVariable("NIXSCP_PREFERENCES_PAGE"));
      QString Search = qEnvironmentVariable("NIXSCP_PREFERENCES_SEARCH");
      if (!Search.isEmpty())
      {
        Dialog->Search(Search);
      }
      Dialog->show();
      QTimer::singleShot(500, Dialog, [Dialog, Screenshot]()
      {
        Dialog->grab().save(Screenshot);
        Dialog->close();
        if (qEnvironmentVariableIsSet("NIXSCP_PREFERENCES_QUIT"))
        {
          QApplication::quit();
        }
      });
    }, Qt::QueuedConnection);
  }
}
Q_COREAPP_STARTUP_FUNCTION(PreferencesStartup)

//---------------------------------------------------------------------------
struct PreferencesDialog::Private
{
  CustomCommandsPage * CustomCommands = nullptr;
  QTreeWidget * NavigationTree;
  QStackedWidget * PageControl;
  bool NoUpdate = false;

  CopyParam CopyParams;
  QList<EditorPreferences> EditorList;
  QString PanelFontName;
  int PanelFontSize = 0;
  QString EditorFontName;
  int EditorFontSize = 0;

  // Environment
  QCheckBox * SynchronizeSummaryCheck, * ConfirmOverwritingCheck, * ConfirmDeletingCheck, * ConfirmRecyclingCheck,
    * ConfirmClosingSessionCheck, * DDTransferConfirmationCheck, * ContinueOnErrorCheck, * ConfirmExitOnCompletionCheck,
    * ConfirmResumeCheck, * ConfirmCommandSessionCheck, * ConfirmTransferringCheck;
  QCheckBox * BeepOnFinishCheck;
  QSpinBox * BeepOnFinishAfterEdit;
  QCheckBox * BalloonNotificationsCheck, * FlashTaskbarCheck;
  QLineEdit * SearchEdit;
  QWidget * SearchSheet;
  QListWidget * SearchResultsView;
  QList<QWidget *> SearchResultControls;
  // Interface
  QComboBox * ThemeCombo;
  // Languages
  QListWidget * LanguagesView;
  QLabel * LanguageChangeLabel;
  // Security
  QCheckBox * UseMasterPasswordCheck;
  QPushButton * SetMasterPasswordButton;
  QLabel * VaultLabel;
  // Storage
  QGroupBox * StorageGroup;
  QRadioButton * AutomaticIniFileStorageButton, * CustomIniFileStorageButton;
  QLabel * AutomaticIniFileStorageLabel;
  QLineEdit * CustomIniFileStorageEdit;
  QPushButton * CustomIniFileStorageBrowse;
  QCheckBox * CheckForUpdatesCheck;
  QLabel * UpdateLastCheckLabel;
  QCheckBox * TemporaryDirectoryCleanupCheck, * ConfirmTemporaryDirectoryCleanupCheck,
    * TemporaryDirectoryAppendSessionCheck, * TemporaryDirectoryAppendPathCheck, * TemporaryDirectoryDeterministicCheck;
  bool MoveStorage = false;
  QString MoveStorageCustom;
  bool MoveStorageUseExisting = false;
  // Window
  QRadioButton * PathInCaptionFullButton, * PathInCaptionShortButton, * PathInCaptionNoneButton;
  QCheckBox * MinimizeToTrayCheck, * ExternalSessionInExistingInstanceCheck, * KeepOpenWhenNoSessionCheck,
    * ShowLoginWhenNoSessionCheck, * SessionTabCaptionTruncationCheck, * SessionSilentDisconnectCheck;
  QCheckBox * AutoSaveWorkspaceCheck, * AutoSaveWorkspacePasswordsCheck, * WorkspaceConnectAllCheck;
  QComboBox * AutoWorkspaceCombo;
  // Commander
  QComboBox * NortonLikeModeCombo;
  QCheckBox * SwappedPanelsCheck, * TreeOnLeftCheck, * UseLocationProfilesCheck;
  QComboBox * ExplorerKeyboardShortcutsCombo;
  QCheckBox * CompareByTimeCheck, * CompareBySizeCheck;
  // Panels
  QCheckBox * ShowHiddenFilesCheck, * DefaultDirIsHomeCheck, * PreservePanelStateCheck, * RenameWholeNameCheck,
    * FullRowSelectCheck, * NaturalOrderNumericalSortingCheck, * AlwaysSortDirectoriesByNameCheck;
  QComboBox * FormatSizeBytesCombo, * PanelSearchCombo, * DoubleClickActionCombo;
  QCheckBox * CopyOnDoubleClickConfirmationCheck;
  QCheckBox * PanelFontCheck;
  QLabel * PanelFontLabel;
  QPushButton * PanelFontButton;
  // Remote
  QCheckBox * ShowInaccesibleDirectoriesCheck, * AutoReadDirectoryAfterOpCheck, * RefreshRemotePanelCheck;
  QSpinBox * RefreshRemotePanelIntervalEdit;
  // Local
  QCheckBox * PreserveLocalDirectoryCheck, * SystemContextMenuCheck, * DeleteToRecycleBinCheck;
  // File colors (mask, BGR TColor), Configuration\Interface\FileColors
  QList<QPair<QString, int>> FileColors;
  QTreeWidget * FileColorsView;
  QPushButton * AddFileColorButton, * EditFileColorButton, * RemoveFileColorButton, * UpFileColorButton, * DownFileColorButton;
  // Editors
  QTreeWidget * EditorListView;
  QPushButton * AddEditorButton, * EditEditorButton, * UpEditorButton, * DownEditorButton, * RemoveEditorButton;
  QCheckBox * EditorCheckNotModifiedCheck;
  // Internal editor
  QCheckBox * EditorWordWrapCheck;
  QSpinBox * EditorTabSizeEdit;
  QLabel * EditorFontLabel;
  QComboBox * EditorEncodingCombo;
  int EditorFontColor = 0;
  int EditorBackgroundColor = 0;
  QCheckBox * EditorWarnLargeFileCheck;
  QSpinBox * EditorLargeFileSizeEdit;
  // Transfer
  CopyParamListFrame * CopyParamPresetsFrame;
  QCheckBox * CopyParamAutoSelectNoticeCheck;
  // Drag & Drop
  QCheckBox * DDWarnLackOfTempSpaceCheck;
  QRadioButton * DDSystemTemporaryDirectoryButton, * DDCustomTemporaryDirectoryButton;
  QLineEdit * DDTemporaryDirectoryEdit;
  // Background
  QSpinBox * QueueTransferLimitEdit;
  QCheckBox * QueueCheck, * QueueNoConfirmationCheck, * QueueParallelCheck, * ParallelTransferCheck,
    * EnableQueueByDefaultCheck, * QueueAutoPopupCheck;
  QComboBox * ParallelTransferThresholdCombo, * QueueKeepDoneItemsForCombo;
  QRadioButton * QueueViewShowButton, * QueueViewHideWhenEmptyButton, * QueueViewHideButton;
  // Endurance
  QRadioButton * ResumeOnButton, * ResumeSmartButton, * ResumeOffButton;
  QSpinBox * ResumeThresholdEdit;
  QCheckBox * SessionReopenAutoCheck, * SessionReopenAutoIdleCheck, * SessionReopenAutoStallCheck, * SessionReopenAutoInactiveCheck;
  QSpinBox * SessionReopenAutoEdit, * SessionReopenAutoIdleEdit, * SessionReopenAutoStallEdit, * SessionReopenTimeoutEdit;
  // Network
  QRadioButton * RetrieveExternalIpAddressButton, * CustomExternalIpAddressButton;
  QLineEdit * CustomExternalIpAddressEdit;
  QCheckBox * LocalPortNumberCheck;
  QSpinBox * LocalPortNumberMinEdit, * LocalPortNumberMaxEdit;
  QCheckBox * TryFtpWhenSshFailsCheck;
  // Security
  QCheckBox * SessionRememberPasswordCheck;
  // Logging
  QCheckBox * EnableLoggingCheck;
  QComboBox * LogProtocolCombo;
  QLabel * LogProtocolHintLabel;
  QLineEdit * LogFileNameEdit;
  QRadioButton * LogFileAppendButton, * LogFileOverwriteButton;
  QCheckBox * LogMaxSizeCheck;
  QComboBox * LogMaxSizeCombo;
  QCheckBox * LogMaxSizeCountCheck;
  QSpinBox * LogMaxSizeCountEdit;
  QCheckBox * LogSensitiveCheck;
  QCheckBox * EnableActionsLoggingCheck;
  QLineEdit * ActionsLogFileNameEdit;
  QGroupBox * LoggingGroup;
  QPushButton * LogFileNameBrowse, * ActionsLogFileNameBrowse;
  QLabel * LogFileNameHintText, * ActionsLogFileNameHintText, * LogMaxSizeCountFilesLabel;
  // Applications
  QLineEdit * PuttyPathEdit;
  QCheckBox * PuttyPasswordCheck, * AutoOpenInPuttyCheck, * TelnetForFtpInPuttyCheck;
};

//---------------------------------------------------------------------------
static QWidget * NewSheet(QVBoxLayout *& Layout)
{
  auto * Sheet = new QWidget();
  Layout = new QVBoxLayout(Sheet);
  Layout->setContentsMargins(0, 0, 0, 0);
  return Sheet;
}

PreferencesDialog::PreferencesDialog(QWidget * Parent) :
  QDialog(Parent),
  d(new Private)
{
  setWindowTitle(QStringLiteral("Preferences"));
  resize(720, 600);

  d->NavigationTree = new QTreeWidget();
  d->NavigationTree->setHeaderHidden(true);
  d->NavigationTree->setRootIsDecorated(false);
  d->NavigationTree->setIndentation(14);
  d->NavigationTree->setFixedWidth(150);
  d->NavigationTree->setProperty("NixSCPTranslateItems", true);
  d->PageControl = new QStackedWidget();
  QTreeWidgetItem * LastTop = nullptr;
  auto AddPage = [this, &LastTop](const QString & Caption, QWidget * Sheet, bool Indented)
  {
    int Index = d->PageControl->addWidget(Sheet);
    QTreeWidgetItem * Item = (Indented && LastTop) ? new QTreeWidgetItem(LastTop) : new QTreeWidgetItem(d->NavigationTree);
    if (!Indented)
    {
      LastTop = Item;
    }
    // translated (and retranslated on a language change) by Translator
    Item->setText(0, Caption);
    Item->setData(0, Qt::UserRole, Index);
    Item->setData(0, Qt::UserRole + 1, Caption);
    Sheet->setProperty("PageHelpKeyword", PreferencesPageHelpKeyword(Caption));
  };
  QVBoxLayout * L;

  // ---- Environment ----
  {
    QWidget * Sheet = NewSheet(L);
    auto * G = new QGroupBox(QStringLiteral("Confirmations"));
    auto * Grid = new QGridLayout(G);
    d->ConfirmOverwritingCheck = new QCheckBox(QStringLiteral("&Overwriting of files"));
    d->ConfirmDeletingCheck = new QCheckBox(QStringLiteral("&Deleting of files (recommended)"));
    d->ConfirmRecyclingCheck = new QCheckBox(QStringLiteral("&Moving files to Recycle bin"));
    d->ConfirmClosingSessionCheck = new QCheckBox(QStringLiteral("Closing sessions when exiting appli&cation"));
    d->ConfirmTransferringCheck = new QCheckBox(QStringLiteral("&Transferring of files"));
    d->DDTransferConfirmationCheck = new QCheckBox(QStringLiteral("D&rag && drop operations and paste to other applications"));
    d->ConfirmExitOnCompletionCheck = new QCheckBox(QStringLiteral("Exiting application on o&peration completion"));
    d->ConfirmResumeCheck = new QCheckBox(QStringLiteral("Trans&fer resuming"));
    d->ConfirmCommandSessionCheck = new QCheckBox(QStringLiteral("Opening separate &shell session"));
    d->SynchronizeSummaryCheck = new QCheckBox(QStringLiteral("Sync&hronization summary"));
    d->ContinueOnErrorCheck = new QCheckBox(QStringLiteral("Continue on &error (advanced users)"));
    Grid->addWidget(d->ConfirmOverwritingCheck, 0, 0);
    Grid->addWidget(d->ConfirmDeletingCheck, 1, 0);
    Grid->addWidget(d->ConfirmRecyclingCheck, 2, 0);
    Grid->addWidget(d->ConfirmClosingSessionCheck, 3, 0);
    Grid->addWidget(d->ConfirmTransferringCheck, 4, 0);
    Grid->addLayout(Indent(d->DDTransferConfirmationCheck), 5, 0, 1, 2);
    Grid->addWidget(d->ConfirmExitOnCompletionCheck, 6, 0);
    Grid->addWidget(d->ConfirmResumeCheck, 0, 1);
    Grid->addWidget(d->ConfirmCommandSessionCheck, 1, 1);
    Grid->addWidget(d->SynchronizeSummaryCheck, 2, 1);
    Grid->addWidget(d->ContinueOnErrorCheck, 7, 0, 1, 2);
    L->addWidget(G);

    auto * N = new QGroupBox(QStringLiteral("Notifications"));
    auto * NV = new QVBoxLayout(N);
    d->BeepOnFinishCheck = new QCheckBox(QStringLiteral("&Beep when work finishes, if it lasted more than"));
    d->BeepOnFinishAfterEdit = SpinBox(0, 9999);
    auto * BeepRow = new QHBoxLayout();
    BeepRow->addWidget(d->BeepOnFinishCheck);
    BeepRow->addWidget(d->BeepOnFinishAfterEdit);
    BeepRow->addWidget(new QLabel(QStringLiteral("s")));
    BeepRow->addStretch();
    NV->addLayout(BeepRow);
    d->BalloonNotificationsCheck = new QCheckBox(QStringLiteral("Show balloon &notifications in taskbar status area (system tray)"));
    d->FlashTaskbarCheck = new QCheckBox(QStringLiteral("Flash taskbar button when &attention is needed while on background"));
    NV->addWidget(d->BalloonNotificationsCheck);
    NV->addWidget(d->FlashTaskbarCheck);
    L->addWidget(N);
    L->addStretch();
    AddPage(QStringLiteral("Environment"), Sheet, false);
  }

  // ---- Environment > Interface ----
  {
    QWidget * Sheet = NewSheet(L);
    auto * G = new QGroupBox(QStringLiteral("Theme"));
    auto * Row = new QHBoxLayout(G);
    d->ThemeCombo = new QComboBox();
    d->ThemeCombo->addItems({QStringLiteral("Automatic"), QStringLiteral("Light"), QStringLiteral("Dark")});
    Row->addWidget(Buddy(QStringLiteral("Interface &theme:"), d->ThemeCombo));
    Row->addWidget(d->ThemeCombo);
    Row->addStretch();
    L->addWidget(G);
    // the theme is applied immediately (WinSCP: "Changes will apply on the next start.")
    L->addStretch();
    AddPage(QStringLiteral("Interface"), Sheet, true);
  }

  // ---- Environment > Window ----
  {
    QWidget * Sheet = NewSheet(L);
    auto * G = new QGroupBox(QStringLiteral("Path in window title"));
    auto * V = new QVBoxLayout(G);
    d->PathInCaptionFullButton = new QRadioButton(QStringLiteral("Show &full path"));
    d->PathInCaptionShortButton = new QRadioButton(QStringLiteral("Sho&w short path"));
    d->PathInCaptionNoneButton = new QRadioButton(QStringLiteral("Do &not show"));
    V->addWidget(d->PathInCaptionFullButton);
    V->addWidget(d->PathInCaptionShortButton);
    V->addWidget(d->PathInCaptionNoneButton);
    L->addWidget(G);

    auto * M = new QGroupBox(QStringLiteral("Miscellaneous"));
    auto * MV = new QVBoxLayout(M);
    d->MinimizeToTrayCheck = new QCheckBox(QStringLiteral("&Minimize main window to taskbar status area (system tray)"));
    if (!QSystemTrayIcon::isSystemTrayAvailable())
    {
      // e.g. GNOME without the AppIndicator extension
      d->MinimizeToTrayCheck->setEnabled(false);
      d->MinimizeToTrayCheck->setToolTip(QStringLiteral("The desktop has no system tray (status notifier) area."));
    }
    d->ExternalSessionInExistingInstanceCheck = new QCheckBox(QStringLiteral("Open new externally initiated sessions in &existing window"));
    d->ShowLoginWhenNoSessionCheck = new QCheckBox(QStringLiteral("&Show Login dialog on startup and when the last session is closed"));
    d->KeepOpenWhenNoSessionCheck = new QCheckBox(QStringLiteral("&Keep main window open when the last session is closed"));
    d->SessionTabCaptionTruncationCheck = new QCheckBox(QStringLiteral("&Truncate tab titles when they do not fit to window"));
    d->SessionSilentDisconnectCheck = new QCheckBox(QStringLiteral("Do not p&opup error message when idle session is disconnected"));
    for (QCheckBox * C : {d->MinimizeToTrayCheck, d->ExternalSessionInExistingInstanceCheck, d->ShowLoginWhenNoSessionCheck,
           d->KeepOpenWhenNoSessionCheck, d->SessionTabCaptionTruncationCheck, d->SessionSilentDisconnectCheck})
    {
      MV->addWidget(C);
    }
    L->addWidget(M);

    auto * W = new QGroupBox(QStringLiteral("Workspaces"));
    auto * WV = new QGridLayout(W);
    d->AutoSaveWorkspaceCheck = new QCheckBox(QStringLiteral("&Automatically save workspace on exit"));
    // AutoWorkspaceCombo: editable, offers the existing workspaces
    d->AutoWorkspaceCombo = new QComboBox();
    d->AutoWorkspaceCombo->setEditable(true);
    d->AutoWorkspaceCombo->setInsertPolicy(QComboBox::NoInsert);
    d->AutoWorkspaceCombo->addItems(Workspaces::WorkspaceNames(Configuration::Instance().StoredSessions()));
    d->AutoSaveWorkspacePasswordsCheck = new QCheckBox(QStringLiteral("Save &passwords (not recommended)"));
    d->WorkspaceConnectAllCheck = new QCheckBox(QStringLiteral("&Connect all sessions of opened workspace or site folder"));
    WV->addWidget(d->AutoSaveWorkspaceCheck, 0, 0, 1, 2);
    WV->addWidget(Buddy(QStringLiteral("&Default workspace name:"), d->AutoWorkspaceCombo), 1, 0);
    WV->addWidget(d->AutoWorkspaceCombo, 1, 1);
    WV->addLayout(Indent(d->AutoSaveWorkspacePasswordsCheck), 2, 0, 1, 2);
    WV->addWidget(d->WorkspaceConnectAllCheck, 3, 0, 1, 2);
    L->addWidget(W);
    L->addStretch();
    AddPage(QStringLiteral("Window"), Sheet, true);
  }

  // ---- Environment > Commander ----
  {
    QWidget * Sheet = NewSheet(L);
    auto * G = new QGroupBox(QStringLiteral("Panels"));
    auto * Grid = new QGridLayout(G);
    d->NortonLikeModeCombo = new QComboBox();
    d->NortonLikeModeCombo->addItems({QStringLiteral("Never"), QStringLiteral("Mouse only"), QStringLiteral("Mouse and Keyboard")});
    Grid->addWidget(Buddy(QStringLiteral("&Explorer-style selection:"), d->NortonLikeModeCombo), 0, 0);
    Grid->addWidget(d->NortonLikeModeCombo, 0, 1);
    d->SwappedPanelsCheck = new QCheckBox(QStringLiteral("S&wap panels (local on right, remote on left)"));
    d->TreeOnLeftCheck = new QCheckBox(QStringLiteral("Show &directory tree left of file list"));
    Grid->addWidget(d->SwappedPanelsCheck, 1, 0, 1, 2);
    Grid->addWidget(d->TreeOnLeftCheck, 2, 0, 1, 2);
    L->addWidget(G);

    auto * M = new QGroupBox(QStringLiteral("Miscellaneous"));
    auto * MG = new QGridLayout(M);
    d->ExplorerKeyboardShortcutsCombo = new QComboBox();
    d->ExplorerKeyboardShortcutsCombo->addItems({QStringLiteral("Commander"), QStringLiteral("Explorer")});
    MG->addWidget(Buddy(QStringLiteral("&Keyboard shortcuts"), d->ExplorerKeyboardShortcutsCombo), 0, 0);
    MG->addWidget(d->ExplorerKeyboardShortcutsCombo, 0, 1);
    d->UseLocationProfilesCheck = new QCheckBox(QStringLiteral("&Use Location Profiles instead of Directory Bookmarks"));
    MG->addWidget(d->UseLocationProfilesCheck, 1, 0, 1, 2);
    L->addWidget(M);

    auto * C = new QGroupBox(QStringLiteral("Compare directory criteria"));
    auto * CV = new QVBoxLayout(C);
    d->CompareByTimeCheck = new QCheckBox(QStringLiteral("Compare by &time"));
    d->CompareBySizeCheck = new QCheckBox(QStringLiteral("Compare by &size"));
    CV->addWidget(d->CompareByTimeCheck);
    CV->addWidget(d->CompareBySizeCheck);
    L->addWidget(C);
    L->addStretch();
    AddPage(QStringLiteral("Commander"), Sheet, true);
  }

  // ---- Environment > Languages ----
  {
    QWidget * Sheet = NewSheet(L);
    auto * G = new QGroupBox(QStringLiteral("Languages"));
    auto * V = new QVBoxLayout(G);
    d->LanguagesView = new QListWidget();
    d->LanguagesView->setProperty("NixSCPNoTranslate", true);
    V->addWidget(d->LanguagesView);
    d->LanguageChangeLabel = new QLabel(QStringLiteral("Changes will apply on the next start."));
    V->addWidget(d->LanguageChangeLabel);
    L->addWidget(G, 1);
    connect(d->LanguagesView, &QListWidget::currentRowChanged, this, [this]() { UpdateControls(); });
    AddPage(QStringLiteral("Languages"), Sheet, true);
  }

  // ---- Panels ----
  {
    QWidget * Sheet = NewSheet(L);
    auto * G = new QGroupBox(QStringLiteral("Common"));
    auto * Grid = new QGridLayout(G);
    d->ShowHiddenFilesCheck = new QCheckBox(QStringLiteral("&Show hidden files (Ctrl+Alt+H)"));
    d->DefaultDirIsHomeCheck = new QCheckBox(QStringLiteral("Default directory is &home directory"));
    d->PreservePanelStateCheck = new QCheckBox(QStringLiteral("&Remember panels' state when switching sessions"));
    d->RenameWholeNameCheck = new QCheckBox(QStringLiteral("Select &whole name when renaming file"));
    d->FullRowSelectCheck = new QCheckBox(QStringLiteral("&Full row select"));
    d->NaturalOrderNumericalSortingCheck = new QCheckBox(QStringLiteral("Use &natural order numerical sorting"));
    d->AlwaysSortDirectoriesByNameCheck = new QCheckBox(QStringLiteral("Always sort &directories by name"));
    int Row = 0;
    for (QCheckBox * C : {d->ShowHiddenFilesCheck, d->DefaultDirIsHomeCheck, d->PreservePanelStateCheck, d->RenameWholeNameCheck,
           d->FullRowSelectCheck, d->NaturalOrderNumericalSortingCheck, d->AlwaysSortDirectoriesByNameCheck})
    {
      Grid->addWidget(C, Row++, 0, 1, 2);
    }
    d->FormatSizeBytesCombo = new QComboBox();
    d->FormatSizeBytesCombo->addItems({QStringLiteral("Bytes"), QStringLiteral("Kilobytes"), QStringLiteral("Short format")});
    Grid->addWidget(Buddy(QStringLiteral("Show file si&zes in:"), d->FormatSizeBytesCombo), Row, 0);
    Grid->addWidget(d->FormatSizeBytesCombo, Row++, 1);
    d->PanelSearchCombo = new QComboBox();
    d->PanelSearchCombo->addItems({QStringLiteral("Beginning of name only"), QStringLiteral("Any part of name"), QStringLiteral("All columns")});
    Grid->addWidget(Buddy(QStringLiteral("&Incremental search:"), d->PanelSearchCombo), Row, 0);
    Grid->addWidget(d->PanelSearchCombo, Row++, 1);
    L->addWidget(G);

    auto * DC = new QGroupBox(QStringLiteral("Double-click"));
    auto * DG = new QGridLayout(DC);
    d->DoubleClickActionCombo = new QComboBox();
    d->DoubleClickActionCombo->addItems({QStringLiteral("Open"), QStringLiteral("Copy"), QStringLiteral("Edit")});
    DG->addWidget(Buddy(QStringLiteral("&Operation to perform on double-click:"), d->DoubleClickActionCombo), 0, 0);
    DG->addWidget(d->DoubleClickActionCombo, 0, 1);
    d->CopyOnDoubleClickConfirmationCheck = new QCheckBox(QStringLiteral("&Confirm copy on double-click operation"));
    DG->addWidget(d->CopyOnDoubleClickConfirmationCheck, 1, 0, 1, 2);
    L->addWidget(DC);

    auto * F = new QGroupBox(QStringLiteral("Panel font"));
    auto * FG = new QGridLayout(F);
    d->PanelFontCheck = new QCheckBox(QStringLiteral("&Use custom font"));
    d->PanelFontButton = new QPushButton(QStringLiteral("Select fon&t..."));
    d->PanelFontLabel = new QLabel();
    FG->addWidget(d->PanelFontCheck, 0, 0);
    FG->addWidget(d->PanelFontButton, 0, 1);
    FG->addWidget(d->PanelFontLabel, 1, 0, 1, 2);
    L->addWidget(F);
    L->addStretch();
    connect(d->PanelFontButton, &QPushButton::clicked, this, [this]()
    {
      bool Ok;
      QFont Initial = font();
      if (!d->PanelFontName.isEmpty())
      {
        Initial = QFont(d->PanelFontName, (d->PanelFontSize > 0) ? d->PanelFontSize : -1);
      }
      QFont F = QFontDialog::getFont(&Ok, Initial, this);
      if (Ok)
      {
        d->PanelFontName = F.family();
        d->PanelFontSize = F.pointSize();
        d->PanelFontCheck->setChecked(true);
        UpdateControls();
      }
    });
    AddPage(QStringLiteral("Panels"), Sheet, false);
  }

  // ---- Panels > File colors ----
  {
    QWidget * Sheet = NewSheet(L);
    auto * G = new QGroupBox(QStringLiteral("File colors"));
    auto * Grid = new QGridLayout(G);
    d->FileColorsView = new QTreeWidget();
    d->FileColorsView->setRootIsDecorated(false);
    d->FileColorsView->setHeaderHidden(true);
    d->FileColorsView->setColumnCount(1);
    d->AddFileColorButton = new QPushButton(QStringLiteral("&Add..."));
    d->EditFileColorButton = new QPushButton(QStringLiteral("&Edit..."));
    d->RemoveFileColorButton = new QPushButton(QStringLiteral("&Remove"));
    d->UpFileColorButton = new QPushButton(QStringLiteral("&Up"));
    d->DownFileColorButton = new QPushButton(QStringLiteral("&Down"));
    Grid->addWidget(d->FileColorsView, 0, 0, 6, 1);
    Grid->addWidget(d->AddFileColorButton, 0, 1);
    Grid->addWidget(d->EditFileColorButton, 1, 1);
    Grid->addWidget(d->RemoveFileColorButton, 2, 1);
    Grid->addWidget(d->UpFileColorButton, 3, 1);
    Grid->addWidget(d->DownFileColorButton, 4, 1);
    Grid->setRowStretch(5, 1);
    L->addWidget(G, 1);
    // TPreferencesDialog::AddEditFileColor
    auto AddEdit = [this](bool Edit)
    {
      int Index = d->FileColorsView->indexOfTopLevelItem(d->FileColorsView->currentItem());
      QString Mask = QStringLiteral("*.*");
      int Color = 0;
      if (Edit)
      {
        if (Index < 0)
        {
          return;
        }
        Mask = d->FileColors[Index].first;
        Color = d->FileColors[Index].second;
      }
      if (EditMaskDialog::ExecuteFileColor(this, Mask, Color))
      {
        if (Edit)
        {
          d->FileColors[Index] = qMakePair(Mask, Color);
        }
        else if (Index < 0)
        {
          d->FileColors.append(qMakePair(Mask, Color));
          Index = d->FileColors.size() - 1;
        }
        else
        {
          d->FileColors.insert(Index, qMakePair(Mask, Color));
        }
        UpdateFileColorsView();
        d->FileColorsView->setCurrentItem(d->FileColorsView->topLevelItem(Index));
        UpdateControls();
      }
    };
    connect(d->AddFileColorButton, &QPushButton::clicked, this, [AddEdit]() { AddEdit(false); });
    connect(d->EditFileColorButton, &QPushButton::clicked, this, [AddEdit]() { AddEdit(true); });
    connect(d->FileColorsView, &QTreeWidget::itemDoubleClicked, this, [AddEdit]() { AddEdit(true); });
    connect(d->RemoveFileColorButton, &QPushButton::clicked, this, [this]()
    {
      int Index = d->FileColorsView->indexOfTopLevelItem(d->FileColorsView->currentItem());
      if (Index >= 0)
      {
        d->FileColors.removeAt(Index);
        UpdateFileColorsView();
        if (!d->FileColors.isEmpty())
        {
          d->FileColorsView->setCurrentItem(d->FileColorsView->topLevelItem(std::min(Index, static_cast<int>(d->FileColors.size()) - 1)));
        }
        UpdateControls();
      }
    });
    auto MoveFileColor = [this](int Delta)
    {
      int Index = d->FileColorsView->indexOfTopLevelItem(d->FileColorsView->currentItem());
      int Target = Index + Delta;
      if ((Index >= 0) && (Target >= 0) && (Target < d->FileColors.size()))
      {
        d->FileColors.move(Index, Target);
        UpdateFileColorsView();
        d->FileColorsView->setCurrentItem(d->FileColorsView->topLevelItem(Target));
        UpdateControls();
      }
    };
    connect(d->UpFileColorButton, &QPushButton::clicked, this, [MoveFileColor]() { MoveFileColor(-1); });
    connect(d->DownFileColorButton, &QPushButton::clicked, this, [MoveFileColor]() { MoveFileColor(1); });
    connect(d->FileColorsView, &QTreeWidget::currentItemChanged, this, [this]() { UpdateControls(); });
    AddPage(QStringLiteral("File colors"), Sheet, true);
  }

  // ---- Panels > Remote ----
  {
    QWidget * Sheet = NewSheet(L);
    auto * G = new QGroupBox(QStringLiteral("Remote panel"));
    auto * V = new QVBoxLayout(G);
    d->ShowInaccesibleDirectoriesCheck = new QCheckBox(QStringLiteral("Show in&accessible directories"));
    d->AutoReadDirectoryAfterOpCheck = new QCheckBox(QStringLiteral("Auto&matically refresh directory after operation (Ctrl+Alt+R)"));
    d->RefreshRemotePanelCheck = new QCheckBox(QStringLiteral("Refresh remote panel &every"));
    d->RefreshRemotePanelIntervalEdit = SpinBox(1, 9999);
    V->addWidget(d->ShowInaccesibleDirectoriesCheck);
    V->addWidget(d->AutoReadDirectoryAfterOpCheck);
    auto * Row = new QHBoxLayout();
    Row->addWidget(d->RefreshRemotePanelCheck);
    Row->addWidget(d->RefreshRemotePanelIntervalEdit);
    Row->addWidget(new QLabel(QStringLiteral("s")));
    Row->addStretch();
    V->addLayout(Row);
    L->addWidget(G);
    L->addStretch();
    AddPage(QStringLiteral("Remote"), Sheet, true);
  }

  // ---- Panels > Local ----
  {
    QWidget * Sheet = NewSheet(L);
    auto * G = new QGroupBox(QStringLiteral("Local panel"));
    auto * V = new QVBoxLayout(G);
    d->PreserveLocalDirectoryCheck = new QCheckBox(QStringLiteral("Do not &change state when switching sessions"));
    d->SystemContextMenuCheck = new QCheckBox(QStringLiteral("Use &system file context menu"));
    d->DeleteToRecycleBinCheck = new QCheckBox(QStringLiteral("&Delete files to recycle bin"));
    V->addWidget(d->PreserveLocalDirectoryCheck);
    V->addWidget(d->SystemContextMenuCheck);
    V->addWidget(d->DeleteToRecycleBinCheck);
    L->addWidget(G);
    L->addStretch();
    AddPage(QStringLiteral("Local"), Sheet, true);
  }

  // ---- Editors ----
  {
    QWidget * Sheet = NewSheet(L);
    auto * G = new QGroupBox(QStringLiteral("Editor preference"));
    auto * Grid = new QGridLayout(G);
    d->EditorListView = new QTreeWidget();
    d->EditorListView->setRootIsDecorated(false);
    d->EditorListView->setHeaderLabels({QStringLiteral("Editor"), QStringLiteral("Mask"), QStringLiteral("Text")});
    d->EditorListView->setColumnWidth(0, 200);
    d->EditorListView->setColumnWidth(1, 150);
    Grid->addWidget(d->EditorListView, 0, 0, 6, 1);
    d->AddEditorButton = new QPushButton(QStringLiteral("&Add..."));
    d->EditEditorButton = new QPushButton(QStringLiteral("&Edit..."));
    d->RemoveEditorButton = new QPushButton(QStringLiteral("&Remove"));
    d->UpEditorButton = new QPushButton(QStringLiteral("&Up"));
    d->DownEditorButton = new QPushButton(QStringLiteral("&Down"));
    Grid->addWidget(d->AddEditorButton, 0, 1);
    Grid->addWidget(d->EditEditorButton, 1, 1);
    Grid->addWidget(d->RemoveEditorButton, 2, 1);
    Grid->addWidget(d->UpEditorButton, 3, 1);
    Grid->addWidget(d->DownEditorButton, 4, 1);
    Grid->setRowStretch(5, 1);
    L->addWidget(G, 1);
    auto * O = new QGroupBox(QStringLiteral("Editing options"));
    auto * OV = new QVBoxLayout(O);
    d->EditorCheckNotModifiedCheck = new QCheckBox(QStringLiteral("&Check that edited remote file has not changed before saving it"));
    OV->addWidget(d->EditorCheckNotModifiedCheck);
    L->addWidget(O);

    connect(d->EditorListView, &QTreeWidget::currentItemChanged, this, [this]() { UpdateControls(); });
    connect(d->EditorListView, &QTreeWidget::itemDoubleClicked, this, [this]()
    {
      EditEditor(d->EditorListView->indexOfTopLevelItem(d->EditorListView->currentItem()));
    });
    connect(d->AddEditorButton, &QPushButton::clicked, this, [this]() { EditEditor(-1); });
    connect(d->EditEditorButton, &QPushButton::clicked, this, [this]()
    {
      EditEditor(d->EditorListView->indexOfTopLevelItem(d->EditorListView->currentItem()));
    });
    connect(d->RemoveEditorButton, &QPushButton::clicked, this, [this]()
    {
      int Index = d->EditorListView->indexOfTopLevelItem(d->EditorListView->currentItem());
      if (Index >= 0)
      {
        d->EditorList.removeAt(Index);
        UpdateEditorListView();
        if (!d->EditorList.isEmpty())
        {
          d->EditorListView->setCurrentItem(d->EditorListView->topLevelItem(qMin(Index, static_cast<int>(d->EditorList.size()) - 1)));
        }
        UpdateControls();
      }
    });
    auto Move = [this](int Delta)
    {
      int Index = d->EditorListView->indexOfTopLevelItem(d->EditorListView->currentItem());
      int Target = Index + Delta;
      if ((Index >= 0) && (Target >= 0) && (Target < d->EditorList.size()))
      {
        d->EditorList.swapItemsAt(Index, Target);
        UpdateEditorListView();
        d->EditorListView->setCurrentItem(d->EditorListView->topLevelItem(Target));
      }
    };
    connect(d->UpEditorButton, &QPushButton::clicked, this, [Move]() { Move(-1); });
    connect(d->DownEditorButton, &QPushButton::clicked, this, [Move]() { Move(1); });
    AddPage(QStringLiteral("Editors"), Sheet, false);
  }

  // ---- Editors > Internal editor ----
  {
    QWidget * Sheet = NewSheet(L);
    auto * G = new QGroupBox(QStringLiteral("Display"));
    auto * Grid = new QGridLayout(G);
    d->EditorWordWrapCheck = new QCheckBox(QStringLiteral("&Wrap long lines"));
    d->EditorTabSizeEdit = SpinBox(1, 99);
    // EditorEncodingCombo: DefaultEncodingName (CP_ACP) and UTF8_NAME (CP_UTF8)
    d->EditorEncodingCombo = new QComboBox();
    d->EditorEncodingCombo->addItem(QStringLiteral("System default (%1)").arg(EditorWindow::EncodingName(EditorWindow::encSystem)));
    d->EditorEncodingCombo->addItem(QStringLiteral("UTF-8"));
    Grid->addWidget(d->EditorWordWrapCheck, 0, 0, 1, 2);
    Grid->addWidget(Buddy(QStringLiteral("&Tabulator size:"), d->EditorTabSizeEdit), 1, 0);
    Grid->addWidget(d->EditorTabSizeEdit, 1, 1);
    Grid->addWidget(Buddy(QStringLiteral("Default en&coding:"), d->EditorEncodingCombo), 2, 0);
    Grid->addWidget(d->EditorEncodingCombo, 2, 1);
    Grid->setColumnStretch(2, 1);
    L->addWidget(G);
    auto * F = new QGroupBox(QStringLiteral("Font"));
    auto * FV = new QHBoxLayout(F);
    d->EditorFontLabel = new QLabel();
    d->EditorFontLabel->setAutoFillBackground(true);
    auto * FontButton = new QPushButton(QStringLiteral("&Select font..."));
    auto * FontColorButton = new QPushButton(QStringLiteral("&Text color"));
    auto * BackgroundColorButton = new QPushButton(QStringLiteral("Default &background"));
    auto * FB = new QVBoxLayout();
    FB->addWidget(FontButton);
    FB->addWidget(FontColorButton);
    FB->addWidget(BackgroundColorButton);
    FB->addStretch();
    FV->addWidget(d->EditorFontLabel, 1);
    FV->addLayout(FB);
    L->addWidget(F);
    // CreateColorPopupMenu / CreateEditorBackgroundColorMenu: Default + custom color
    auto ColorMenu = [this](QPushButton * Button, int * Color, const QString & Title)
    {
      auto * Menu = new QMenu(Button);
      QAction * DefaultAction = Menu->addAction(QStringLiteral("&Default"));
      DefaultAction->setCheckable(true);
      QAction * MoreAction = Menu->addAction(QStringLiteral("&More Colors..."));
      connect(Menu, &QMenu::aboutToShow, this, [DefaultAction, Color]() { DefaultAction->setChecked(*Color == 0); });
      connect(DefaultAction, &QAction::triggered, this, [this, Color]()
      {
        *Color = 0;
        UpdateControls();
      });
      connect(MoreAction, &QAction::triggered, this, [this, Color, Title]()
      {
        QColor Initial = (*Color > 0) ? QColor(*Color & 0xFF, (*Color >> 8) & 0xFF, (*Color >> 16) & 0xFF) : QColor(Qt::white);
        QColor Selected = QColorDialog::getColor(Initial, this, Title);
        if (Selected.isValid())
        {
          // BGR TColor; pure black would be "default" (0), as with WinSCP's clBlack
          *Color = std::max(1, Selected.red() | (Selected.green() << 8) | (Selected.blue() << 16));
          UpdateControls();
        }
      });
      Button->setMenu(Menu);
    };
    ColorMenu(FontColorButton, &d->EditorFontColor, QStringLiteral("Text color"));
    ColorMenu(BackgroundColorButton, &d->EditorBackgroundColor, QStringLiteral("Default background"));
    connect(FontButton, &QPushButton::clicked, this, [this]()
    {
      bool Ok;
      QFont Initial = QFontDatabase::systemFont(QFontDatabase::FixedFont);
      if (!d->EditorFontName.isEmpty())
      {
        Initial = QFont(d->EditorFontName, (d->EditorFontSize > 0) ? d->EditorFontSize : -1);
      }
      QFont Font = QFontDialog::getFont(&Ok, Initial, this, QString(), QFontDialog::MonospacedFonts);
      if (Ok)
      {
        d->EditorFontName = Font.family();
        d->EditorFontSize = Font.pointSize();
        UpdateControls();
      }
    });
    auto * B = new QGroupBox(QStringLiteral("Behaviour"));
    auto * BRow = new QHBoxLayout(B);
    d->EditorWarnLargeFileCheck = new QCheckBox(QStringLiteral("Warn when &editing files larger than:"));
    d->EditorLargeFileSizeEdit = SpinBox(1, 2000000000);
    BRow->addWidget(d->EditorWarnLargeFileCheck);
    BRow->addWidget(d->EditorLargeFileSizeEdit);
    BRow->addWidget(new QLabel(QStringLiteral("KB")));
    BRow->addStretch();
    L->addWidget(B);
    L->addStretch();
    AddPage(QStringLiteral("Internal editor"), Sheet, true);
  }

  // ---- Transfer ----
  {
    // CopyParamListSheet: transfer settings presets (Default + the presets)
    QWidget * Sheet = NewSheet(L);
    d->CopyParamPresetsFrame = new CopyParamListFrame(&d->CopyParams);
    d->CopyParamAutoSelectNoticeCheck = d->CopyParamPresetsFrame->AutoSelectNoticeCheck();
    L->addWidget(d->CopyParamPresetsFrame, 1);
    AddPage(QStringLiteral("Transfer"), Sheet, false);
  }

  // ---- Transfer > Drag & Drop ----
  {
    QWidget * Sheet = NewSheet(L);
    auto * G = new QGroupBox(QStringLiteral("Drag && Drop downloads"));
    auto * V = new QVBoxLayout(G);
    d->DDWarnLackOfTempSpaceCheck = new QCheckBox(QStringLiteral("&Warn when there is not enough free space"));
    V->addWidget(d->DDWarnLackOfTempSpaceCheck);
    L->addWidget(G);
    auto * T = new QGroupBox(QStringLiteral("Temporary directory"));
    auto * TG = new QGridLayout(T);
    TG->addWidget(new QLabel(QStringLiteral("Specify where to temporarily store edited and downloaded files.")), 0, 0, 1, 2);
    d->DDSystemTemporaryDirectoryButton = new QRadioButton(QStringLiteral("&Use temporary directory of system"));
    d->DDCustomTemporaryDirectoryButton = new QRadioButton(QStringLiteral("Use this &directory:"));
    d->DDTemporaryDirectoryEdit = new QLineEdit();
    auto * Browse = new QPushButton(QStringLiteral("..."));
    Browse->setMaximumWidth(32);
    TG->addWidget(d->DDSystemTemporaryDirectoryButton, 1, 0, 1, 2);
    TG->addWidget(d->DDCustomTemporaryDirectoryButton, 2, 0, 1, 2);
    TG->addWidget(d->DDTemporaryDirectoryEdit, 3, 0);
    TG->addWidget(Browse, 3, 1);
    L->addWidget(T);
    L->addStretch();
    connect(Browse, &QPushButton::clicked, this, [this]()
    {
      QString Dir = QFileDialog::getExistingDirectory(this, QString(), d->DDTemporaryDirectoryEdit->text());
      if (!Dir.isEmpty())
      {
        d->DDTemporaryDirectoryEdit->setText(Dir);
        d->DDCustomTemporaryDirectoryButton->setChecked(true);
      }
    });
    AddPage(QStringLiteral("Drag & Drop"), Sheet, true);
  }

  // ---- Transfer > Background ----
  {
    QWidget * Sheet = NewSheet(L);
    auto * G = new QGroupBox(QStringLiteral("Background transfers"));
    auto * Grid = new QGridLayout(G);
    d->QueueTransferLimitEdit = SpinBox(1, 9);
    Grid->addWidget(Buddy(QStringLiteral("&Maximal number of transfers at the same time:"), d->QueueTransferLimitEdit), 0, 0);
    Grid->addWidget(d->QueueTransferLimitEdit, 0, 1);
    d->QueueParallelCheck = new QCheckBox(QStringLiteral("&Use multiple connections for single transfer"));
    Grid->addWidget(d->QueueParallelCheck, 1, 0, 1, 2);
    d->ParallelTransferCheck = new QCheckBox(QStringLiteral("Use multiple connections for single files abo&ve:"));
    d->ParallelTransferThresholdCombo = new QComboBox();
    d->ParallelTransferThresholdCombo->setEditable(true);
    d->ParallelTransferThresholdCombo->addItems({QStringLiteral("1M"), QStringLiteral("10M"), QStringLiteral("100M"), QStringLiteral("1G")});
    auto * PRow = new QHBoxLayout();
    PRow->addSpacing(20);
    PRow->addWidget(d->ParallelTransferCheck);
    PRow->addWidget(d->ParallelTransferThresholdCombo);
    PRow->addWidget(new QLabel(QStringLiteral("bytes")));
    Grid->addLayout(PRow, 2, 0, 1, 2);
    d->QueueCheck = new QCheckBox(QStringLiteral("Transfer in &background by default"));
    d->QueueNoConfirmationCheck = new QCheckBox(QStringLiteral("&No confirmations for background transfers"));
    d->QueueAutoPopupCheck = new QCheckBox(QStringLiteral("&Automatically popup prompts of background transfers when idle"));
    d->EnableQueueByDefaultCheck = new QCheckBox(QStringLiteral("&Enable queue processing by default"));
    Grid->addWidget(d->QueueCheck, 3, 0, 1, 2);
    Grid->addWidget(d->QueueNoConfirmationCheck, 4, 0, 1, 2);
    Grid->addWidget(d->QueueAutoPopupCheck, 5, 0, 1, 2);
    Grid->addWidget(d->EnableQueueByDefaultCheck, 6, 0, 1, 2);
    d->QueueKeepDoneItemsForCombo = new QComboBox();
    d->QueueKeepDoneItemsForCombo->addItems({QStringLiteral("Never"), QStringLiteral("15 seconds"), QStringLiteral("1 minute"),
      QStringLiteral("15 minutes"), QStringLiteral("1 hour"), QStringLiteral("Forever")});
    Grid->addWidget(Buddy(QStringLiteral("Display &completed transfers in queue for:"), d->QueueKeepDoneItemsForCombo), 7, 0);
    Grid->addWidget(d->QueueKeepDoneItemsForCombo, 7, 1);
    L->addWidget(G);
    auto * Q = new QGroupBox(QStringLiteral("Queue list"));
    auto * QV = new QVBoxLayout(Q);
    d->QueueViewShowButton = new QRadioButton(QStringLiteral("&Show"));
    d->QueueViewHideWhenEmptyButton = new QRadioButton(QStringLiteral("Hide &when empty"));
    d->QueueViewHideButton = new QRadioButton(QStringLiteral("&Hide"));
    QV->addWidget(d->QueueViewShowButton);
    QV->addWidget(d->QueueViewHideWhenEmptyButton);
    QV->addWidget(d->QueueViewHideButton);
    L->addWidget(Q);
    L->addStretch();
    AddPage(QStringLiteral("Background"), Sheet, true);
  }

  // ---- Transfer > Endurance ----
  {
    QWidget * Sheet = NewSheet(L);
    auto * G = new QGroupBox(QStringLiteral("Enable transfer resume/transfer to temporary filename for"));
    auto * V = new QVBoxLayout(G);
    d->ResumeOnButton = new QRadioButton(QStringLiteral("A&ll files"));
    d->ResumeSmartButton = new QRadioButton(QStringLiteral("Files abo&ve:"));
    d->ResumeThresholdEdit = SpinBox(0, 2000000000);
    d->ResumeOffButton = new QRadioButton(QStringLiteral("Di&sable"));
    V->addWidget(d->ResumeOnButton);
    auto * Row = new QHBoxLayout();
    Row->addWidget(d->ResumeSmartButton);
    Row->addWidget(d->ResumeThresholdEdit);
    Row->addWidget(new QLabel(QStringLiteral("KB")));
    Row->addStretch();
    V->addLayout(Row);
    V->addWidget(d->ResumeOffButton);
    L->addWidget(G);

    auto * R = new QGroupBox(QStringLiteral("Automatic reconnect"));
    auto * Grid = new QGridLayout(R);
    auto AddReconnect = [Grid](int Row, QCheckBox * Check, QSpinBox * Edit, const QString & Caption)
    {
      Grid->addWidget(Check, Row, 0, 1, 4);
      Grid->addItem(new QSpacerItem(20, 1), Row + 1, 0);
      Grid->addWidget(Buddy(Caption, Edit), Row + 1, 1);
      Grid->addWidget(Edit, Row + 1, 2);
      Grid->addWidget(new QLabel(QStringLiteral("seconds")), Row + 1, 3);
    };
    d->SessionReopenAutoCheck = new QCheckBox(QStringLiteral("&Automatically reconnect session, if it breaks during transfer"));
    d->SessionReopenAutoEdit = SpinBox(1, 86400);
    AddReconnect(0, d->SessionReopenAutoCheck, d->SessionReopenAutoEdit, QStringLiteral("&Reconnect after:"));
    d->SessionReopenAutoIdleCheck = new QCheckBox(QStringLiteral("Automatically reconnect session, if it breaks &while idle"));
    d->SessionReopenAutoIdleEdit = SpinBox(1, 86400);
    AddReconnect(2, d->SessionReopenAutoIdleCheck, d->SessionReopenAutoIdleEdit, QStringLiteral("Reco&nnect after:"));
    d->SessionReopenAutoStallCheck = new QCheckBox(QStringLiteral("Automatically reconnect session, if it &stalls"));
    d->SessionReopenAutoStallEdit = SpinBox(1, 86400);
    AddReconnect(4, d->SessionReopenAutoStallCheck, d->SessionReopenAutoStallEdit, QStringLiteral("Re&connect after:"));
    d->SessionReopenAutoInactiveCheck = new QCheckBox(QStringLiteral("Automatically reconnect session, if it breaks &while inactive"));
    Grid->addWidget(d->SessionReopenAutoInactiveCheck, 6, 0, 1, 4);
    d->SessionReopenTimeoutEdit = SpinBox(0, 86400);
    d->SessionReopenTimeoutEdit->setSpecialValueText(QStringLiteral("Forever"));
    Grid->addWidget(Buddy(QStringLiteral("&Keep reconnecting for:"), d->SessionReopenTimeoutEdit), 7, 0, 1, 2);
    Grid->addWidget(d->SessionReopenTimeoutEdit, 7, 2);
    Grid->addWidget(new QLabel(QStringLiteral("seconds")), 7, 3);
    Grid->setColumnStretch(4, 1);
    L->addWidget(R);
    L->addStretch();
    AddPage(QStringLiteral("Endurance"), Sheet, true);
  }

  // ---- Network ----
  {
    QWidget * Sheet = NewSheet(L);
    auto * G = new QGroupBox(QStringLiteral("Incoming FTP connections (active mode)"));
    auto * Grid = new QGridLayout(G);
    d->RetrieveExternalIpAddressButton = new QRadioButton(QStringLiteral("Retrieve the external IP address from &operating system"));
    d->CustomExternalIpAddressButton = new QRadioButton(QStringLiteral("Use the &following external IP address:"));
    d->CustomExternalIpAddressEdit = new QLineEdit();
    d->LocalPortNumberCheck = new QCheckBox(QStringLiteral("Limit listening &ports to:"));
    d->LocalPortNumberMinEdit = SpinBox(1024, 65535);
    d->LocalPortNumberMaxEdit = SpinBox(1024, 65535);
    Grid->addWidget(d->RetrieveExternalIpAddressButton, 0, 0, 1, 4);
    Grid->addWidget(d->CustomExternalIpAddressButton, 1, 0, 1, 4);
    Grid->addLayout(Indent(d->CustomExternalIpAddressEdit), 2, 0, 1, 4);
    Grid->addWidget(d->LocalPortNumberCheck, 3, 0);
    Grid->addWidget(d->LocalPortNumberMinEdit, 3, 1);
    Grid->addWidget(new QLabel(QString(QChar(0x2013))), 3, 2);
    Grid->addWidget(d->LocalPortNumberMaxEdit, 3, 3);
    L->addWidget(G);
    auto * C = new QGroupBox(QStringLiteral("Connections"));
    auto * CV = new QVBoxLayout(C);
    d->TryFtpWhenSshFailsCheck = new QCheckBox(QStringLiteral("When SFTP connection is &rejected, knock FTP port"));
    CV->addWidget(d->TryFtpWhenSshFailsCheck);
    L->addWidget(C);
    L->addStretch();
    AddPage(QStringLiteral("Network"), Sheet, false);
  }

  // ---- Security ----
  {
    QWidget * Sheet = NewSheet(L);
    auto * M = new QGroupBox(QStringLiteral("Master password"));
    auto * MV = new QVBoxLayout(M);
    auto * MRow = new QHBoxLayout();
    d->UseMasterPasswordCheck = new QCheckBox(QStringLiteral("&Use master password"));
    d->SetMasterPasswordButton = new QPushButton(QStringLiteral("&Change master password..."));
    MRow->addWidget(d->UseMasterPasswordCheck);
    MRow->addStretch();
    MRow->addWidget(d->SetMasterPasswordButton);
    MV->addLayout(MRow);
    // NixSCP Pro: the NixSCP password encrypts the whole configuration instead
    d->VaultLabel = new QLabel(QStringLiteral("The whole configuration is encrypted with your NixSCP password (NixSCP Pro), "
      "which replaces the master password. Change it in Pro &gt; Cloud Sync Settings."));
    d->VaultLabel->setWordWrap(true);
    MV->addWidget(d->VaultLabel);
    L->addWidget(M);
    // takes effect immediately, as in WinSCP (TPreferencesDialog::UseMasterPasswordCheckClick)
    connect(d->UseMasterPasswordCheck, &QCheckBox::clicked, this, [this]() { UseMasterPasswordClicked(); });
    connect(d->SetMasterPasswordButton, &QPushButton::clicked, this, [this]()
    {
      ChangeMasterPassword(QStringLiteral("Master password successfully changed."));
    });
    // Authentication agent (PuTTY Pageant / OpenSSH ssh-agent): ssh-agent only on Linux
    auto * G = new QGroupBox(QStringLiteral("Authentication"));
    auto * V = new QVBoxLayout(G);
    d->SessionRememberPasswordCheck = new QCheckBox(QStringLiteral("Remember &password for duration of session"));
    V->addWidget(d->SessionRememberPasswordCheck);
    L->addWidget(G);
    L->addStretch();
    AddPage(QStringLiteral("Security"), Sheet, false);
  }

  // ---- Logging ----
  {
    QWidget * Sheet = NewSheet(L);
    d->LoggingGroup = new QGroupBox(QStringLiteral("Session log"));
    auto * Grid = new QGridLayout(d->LoggingGroup);
    d->EnableLoggingCheck = new QCheckBox(QStringLiteral("Enable &session logging on level:"));
    d->LogProtocolCombo = new QComboBox();
    d->LogProtocolCombo->addItems({QStringLiteral("Reduced"), QStringLiteral("Normal"), QStringLiteral("Debug 1"), QStringLiteral("Debug 2")});
    Grid->addWidget(d->EnableLoggingCheck, 0, 0, 1, 2);
    Grid->addWidget(d->LogProtocolCombo, 0, 2);
    d->LogFileNameEdit = new QLineEdit();
    d->LogFileNameBrowse = new QPushButton(QStringLiteral("..."));
    d->LogFileNameBrowse->setMaximumWidth(32);
    Grid->addWidget(Buddy(QStringLiteral("&Log path:"), d->LogFileNameEdit), 1, 0);
    auto * PathRow = new QHBoxLayout();
    PathRow->addWidget(d->LogFileNameEdit);
    PathRow->addWidget(d->LogFileNameBrowse);
    Grid->addLayout(PathRow, 1, 1, 1, 3);
    // HintLabel(LogFileNameHintText, LOG_FILE_HINT3): "patterns" link with a hint
    const QString LogFileHint = QStringLiteral(
      "!Y year\n!M month\n!D day\n!T time\n!P process ID\n!@ hostname\n!S session name\nExample: ~/logs/!S!T.log");
    auto NewHintLabel = [&LogFileHint](const QString & Caption)
    {
      auto * Label = new QLabel(QStringLiteral("<a href=\"patterns\">%1</a>").arg(Translator::Tr(Caption)));
      Label->setToolTip(Translator::Tr(LogFileHint));
      Label->setProperty("NixSCPNoTranslate", true);
      connect(Label, &QLabel::linkActivated, Label, [Label]() { QToolTip::showText(QCursor::pos(), Label->toolTip(), Label); });
      return Label;
    };
    d->LogFileNameHintText = NewHintLabel(QStringLiteral("patterns"));
    Grid->addWidget(d->LogFileNameHintText, 2, 1, 1, 3, Qt::AlignRight);
    d->LogFileAppendButton = new QRadioButton(QStringLiteral("Appe&nd"));
    d->LogFileOverwriteButton = new QRadioButton(QStringLiteral("&Overwrite"));
    auto * AppendRow = new QHBoxLayout();
    AppendRow->addWidget(d->LogFileAppendButton);
    AppendRow->addWidget(d->LogFileOverwriteButton);
    AppendRow->addStretch();
    Grid->addLayout(AppendRow, 3, 1, 1, 3);
    d->LogMaxSizeCheck = new QCheckBox(QStringLiteral("&Rotate log files after reaching"));
    d->LogMaxSizeCombo = new QComboBox();
    d->LogMaxSizeCombo->setEditable(true);
    d->LogMaxSizeCombo->addItems({QStringLiteral("1M"), QStringLiteral("10M"), QStringLiteral("100M"), QStringLiteral("1G")});
    Grid->addWidget(d->LogMaxSizeCheck, 4, 0, 1, 2);
    Grid->addWidget(d->LogMaxSizeCombo, 4, 2);
    d->LogMaxSizeCountCheck = new QCheckBox(QStringLiteral("&Delete old log files, keep"));
    d->LogMaxSizeCountEdit = SpinBox(1, 999);
    auto * CountRow = new QHBoxLayout();
    CountRow->addSpacing(20);
    CountRow->addWidget(d->LogMaxSizeCountCheck);
    Grid->addLayout(CountRow, 5, 0, 1, 2);
    auto * CountEditRow = new QHBoxLayout();
    CountEditRow->addWidget(d->LogMaxSizeCountEdit);
    d->LogMaxSizeCountFilesLabel = new QLabel(QStringLiteral("files"));
    CountEditRow->addWidget(d->LogMaxSizeCountFilesLabel);
    Grid->addLayout(CountEditRow, 5, 2);
    d->LogSensitiveCheck = new QCheckBox(QStringLiteral("Log pass&words and other sensitive information"));
    Grid->addWidget(d->LogSensitiveCheck, 6, 0, 1, 4);
    Grid->setColumnStretch(3, 1);
    L->addWidget(d->LoggingGroup);
    d->LogProtocolHintLabel = new QLabel(QStringLiteral("The selected logging level severely degrades performance. Use it when troubleshooting only."));
    d->LogProtocolHintLabel->setWordWrap(true);
    L->addWidget(d->LogProtocolHintLabel);

    auto * X = new QGroupBox(QStringLiteral("XML log"));
    auto * XV = new QVBoxLayout(X);
    d->EnableActionsLoggingCheck = new QCheckBox(QStringLiteral("Enable &XML logging to file:"));
    d->ActionsLogFileNameEdit = new QLineEdit();
    d->ActionsLogFileNameBrowse = new QPushButton(QStringLiteral("..."));
    d->ActionsLogFileNameBrowse->setMaximumWidth(32);
    XV->addWidget(d->EnableActionsLoggingCheck);
    auto * XmlPathRow = new QHBoxLayout();
    XmlPathRow->addWidget(d->ActionsLogFileNameEdit);
    XmlPathRow->addWidget(d->ActionsLogFileNameBrowse);
    XV->addLayout(XmlPathRow);
    d->ActionsLogFileNameHintText = NewHintLabel(QStringLiteral("patterns"));
    XV->addWidget(d->ActionsLogFileNameHintText, 0, Qt::AlignRight);
    L->addWidget(X);
    L->addStretch();
    // TFilenameEdit: the file dialog starts with the expanded path
    auto BrowseLog = [this](QLineEdit * Edit, const QString & Filter)
    {
      QString Current = SessionLog::ExpandFileName(Edit->text(), QDateTime::currentDateTime(), nullptr);
      QString File = QFileDialog::getSaveFileName(this, QString(), Current, Filter, nullptr, QFileDialog::DontConfirmOverwrite);
      if (!File.isEmpty())
      {
        Edit->setText(File);
      }
    };
    connect(d->LogFileNameBrowse, &QPushButton::clicked, this, [this, BrowseLog]()
    {
      BrowseLog(d->LogFileNameEdit, QStringLiteral("Session log files (*.log);;All Files (*)"));
    });
    connect(d->ActionsLogFileNameBrowse, &QPushButton::clicked, this, [this, BrowseLog]()
    {
      BrowseLog(d->ActionsLogFileNameEdit, QStringLiteral("XML log files (*.xml);;All Files (*)"));
    });
    AddPage(QStringLiteral("Logging"), Sheet, false);
  }

  // ---- Integration ----
  {
    QWidget * Sheet = NewSheet(L);
    auto * G = new QGroupBox(QStringLiteral("Desktop integration"));
    auto * V = new QVBoxLayout(G);
    auto * DesktopIconButton = new QPushButton(QStringLiteral("Create a &desktop icon"));
    auto * UrlButton = new QPushButton(QStringLiteral("Register to handle &URL addresses"));
    auto * UrlMenu = new QMenu(this);
    UrlButton->setObjectName(QStringLiteral("RegisterAsUrlHandlersButton"));
    UrlMenu->addAction(QStringLiteral("Register"), this, [this]() { RegisterAsUrlHandler(true); })
      ->setObjectName(QStringLiteral("RegisterAsUrlHandlerItem"));
    UrlMenu->addAction(QStringLiteral("Unregister"), this, [this]() { RegisterAsUrlHandler(false); })
      ->setObjectName(QStringLiteral("UnregisterForDefaultProtocolsItem"));
    UrlButton->setMenu(UrlMenu);
    for (QPushButton * B : {DesktopIconButton, UrlButton})
    {
      auto * Row = new QHBoxLayout();
      Row->addWidget(B);
      Row->addStretch();
      V->addLayout(Row);
    }
    L->addWidget(G);
    L->addStretch();
    connect(DesktopIconButton, &QPushButton::clicked, this, [this]() { CreateDesktopIcon(); });
    AddPage(QStringLiteral("Integration"), Sheet, false);
  }

  // ---- Integration > Applications ----
  {
    QWidget * Sheet = NewSheet(L);
    auto * G = new QGroupBox(QStringLiteral("External applications"));
    auto * Grid = new QGridLayout(G);
    d->PuttyPathEdit = new QLineEdit();
    auto * Browse = new QPushButton(QStringLiteral("B&rowse..."));
    Grid->addWidget(Buddy(QStringLiteral("PuTTY/Terminal &client path:"), d->PuttyPathEdit), 0, 0, 1, 2);
    Grid->addWidget(d->PuttyPathEdit, 1, 0);
    Grid->addWidget(Browse, 1, 1);
    // HintLabel(PuttyPathHintText, PUTTY_PATTERNS_HINT2 + PATTERNS_HINT_K): "patterns" link with a hint
    QString PatternsHint = QStringLiteral(
      "Patterns:\n!! expands to exclamation mark\n!/ expands to current remote path\n"
      "!@ expands to current session hostname\n!U expands to current session username\n"
      "!P expands to current session password\n!# expands to current session port number\n"
      "!N expands to current session name\n!K expands to current session private key path\n"
      "!?prompt[\\]?default! expands to user-entered value with given prompt and default (optional \\ avoids escaping)\n"
      "!`command` expands to output of local command");
    auto * Hint = new QLabel(QStringLiteral("<a href=\"patterns\">%1</a>").arg(Translator::Tr(QStringLiteral("patterns"))));
    Hint->setToolTip(Translator::Tr(PatternsHint));
    Hint->setProperty("NixSCPNoTranslate", true);
    connect(Hint, &QLabel::linkActivated, Hint, [Hint]() { QToolTip::showText(QCursor::pos(), Hint->toolTip(), Hint); });
    Grid->addWidget(Hint, 2, 1, Qt::AlignRight);
    d->PuttyPasswordCheck = new QCheckBox(QStringLiteral("&Remember session password and pass it to PuTTY (SSH)"));
    d->TelnetForFtpInPuttyCheck = new QCheckBox(QStringLiteral("Open &Telnet sessions in PuTTY for FTP sessions"));
    d->AutoOpenInPuttyCheck = new QCheckBox(QStringLiteral("Automatically &open new sessions in PuTTY"));
    Grid->addWidget(d->PuttyPasswordCheck, 3, 0, 1, 2);
    Grid->addWidget(d->TelnetForFtpInPuttyCheck, 4, 0, 1, 2);
    Grid->addWidget(d->AutoOpenInPuttyCheck, 5, 0, 1, 2);
    L->addWidget(G);
    L->addStretch();
    connect(Browse, &QPushButton::clicked, this, [this]()
    {
      QString File = QFileDialog::getOpenFileName(this, QString(), QStringLiteral("/usr/bin"));
      if (!File.isEmpty())
      {
        d->PuttyPathEdit->setText(File);
      }
    });
    AddPage(QStringLiteral("Applications"), Sheet, true);
  }

  // ---- Commands (custom commands, after Integration > Applications as in WinSCP) ----
  {
    d->CustomCommands = new CustomCommandsPage();
    d->CustomCommands->LoadConfiguration();
    AddPage(QStringLiteral("Commands"), d->CustomCommands, false);
  }

  // ---- Storage ----
  {
    QWidget * Sheet = NewSheet(L);
    d->StorageGroup = new QGroupBox(QStringLiteral("Configuration storage"));
    auto * Grid = new QGridLayout(d->StorageGroup);
    // "Windows registry" storage does not exist on Linux
    d->AutomaticIniFileStorageButton = new QRadioButton(QStringLiteral("&Automatic INI file"));
    d->AutomaticIniFileStorageLabel = new QLabel(Configuration::AutomaticIniFileName());
    d->AutomaticIniFileStorageLabel->setProperty("NixSCPNoTranslate", true);
    d->AutomaticIniFileStorageLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    d->CustomIniFileStorageButton = new QRadioButton(QStringLiteral("Custo&m INI file:"));
    d->CustomIniFileStorageEdit = new QLineEdit();
    d->CustomIniFileStorageBrowse = new QPushButton(QStringLiteral("..."));
    d->CustomIniFileStorageBrowse->setMaximumWidth(32);
    Grid->addWidget(d->AutomaticIniFileStorageButton, 0, 0, 1, 2);
    Grid->addLayout(Indent(d->AutomaticIniFileStorageLabel), 1, 0, 1, 2);
    Grid->addWidget(d->CustomIniFileStorageButton, 2, 0, 1, 2);
    Grid->addLayout(Indent(d->CustomIniFileStorageEdit), 3, 0);
    Grid->addWidget(d->CustomIniFileStorageBrowse, 3, 1);
    L->addWidget(d->StorageGroup);
    connect(d->CustomIniFileStorageBrowse, &QPushButton::clicked, this, [this]()
    {
      QString File = QFileDialog::getSaveFileName(this, QString(), d->CustomIniFileStorageEdit->text(),
        QStringLiteral("INI files (*.ini);;All files (*)"), nullptr, QFileDialog::DontConfirmOverwrite);
      if (!File.isEmpty())
      {
        d->CustomIniFileStorageEdit->setText(File);
        d->CustomIniFileStorageButton->setChecked(true);
      }
    });

    // Temporary directory: the group of the Drag & Drop page belongs here, as in WinSCP
    auto * TemporaryGroup = qobject_cast<QGroupBox *>(d->DDTemporaryDirectoryEdit->parentWidget());
    auto * TemporaryGrid = (TemporaryGroup != nullptr) ? qobject_cast<QGridLayout *>(TemporaryGroup->layout()) : nullptr;
    if (TemporaryGrid == nullptr)
    {
      TemporaryGroup = new QGroupBox(QStringLiteral("Temporary directory"));
      TemporaryGrid = new QGridLayout(TemporaryGroup);
    }
    d->TemporaryDirectoryCleanupCheck = new QCheckBox(QStringLiteral("&Cleanup obsolete temporary directories on startup"));
    d->ConfirmTemporaryDirectoryCleanupCheck = new QCheckBox(QStringLiteral("&Ask before cleanup"));
    d->TemporaryDirectoryAppendSessionCheck = new QCheckBox(QStringLiteral("Append &session name to temporary path"));
    d->TemporaryDirectoryAppendPathCheck = new QCheckBox(QStringLiteral("Append remote &path to temporary path"));
    d->TemporaryDirectoryDeterministicCheck = new QCheckBox(QStringLiteral("Keep temporary copies of remote files in &deterministic paths"));
    int Row = TemporaryGrid->rowCount();
    TemporaryGrid->addWidget(d->TemporaryDirectoryCleanupCheck, Row++, 0, 1, 2);
    TemporaryGrid->addLayout(Indent(d->ConfirmTemporaryDirectoryCleanupCheck), Row++, 0, 1, 2);
    TemporaryGrid->addWidget(d->TemporaryDirectoryAppendSessionCheck, Row++, 0, 1, 2);
    TemporaryGrid->addWidget(d->TemporaryDirectoryAppendPathCheck, Row++, 0, 1, 2);
    TemporaryGrid->addWidget(d->TemporaryDirectoryDeterministicCheck, Row++, 0, 1, 2);
    L->addWidget(TemporaryGroup);
    // "Other" (random seed file): PuTTY's random seed is not used
    L->addStretch();
    AddPage(QStringLiteral("Storage"), Sheet, false);
  }

  // ---- Updates (NixSCP: a notice about new releases on GitHub, no self-update) ----
  {
    QWidget * Sheet = NewSheet(L);
    auto * G = new QGroupBox(QStringLiteral("New versions"));
    auto * V = new QVBoxLayout(G);
    d->CheckForUpdatesCheck = new QCheckBox(QStringLiteral("&Check for a new version when NixSCP starts (at most once a day)"));
    V->addWidget(d->CheckForUpdatesCheck);
    auto * Note = new QLabel(QStringLiteral("NixSCP asks GitHub for the latest release and tells you when a newer one is "
      "available. Nothing is downloaded or installed automatically."));
    Note->setWordWrap(true);
    V->addWidget(Note);
    d->UpdateLastCheckLabel = new QLabel();
    V->addWidget(d->UpdateLastCheckLabel);
    auto * Row = new QHBoxLayout();
    auto * CheckNow = new QPushButton(QStringLiteral("Check &now"));
    Row->addWidget(CheckNow);
    Row->addStretch();
    V->addLayout(Row);
    connect(CheckNow, &QPushButton::clicked, this, [this]() { UpdateCheck::CheckNow(this); });
    L->addWidget(G);
    L->addStretch();
    AddPage(QStringLiteral("Updates"), Sheet, false);
  }

  // ---- Search results (not in the navigation tree) ----
  {
    d->SearchSheet = NewSheet(L);
    auto * G = new QGroupBox(QStringLiteral("Search"));
    auto * V = new QVBoxLayout(G);
    d->SearchResultsView = new QListWidget();
    d->SearchResultsView->setProperty("NixSCPNoTranslate", true);
    V->addWidget(d->SearchResultsView);
    L->addWidget(G, 1);
    d->PageControl->addWidget(d->SearchSheet);
    connect(d->SearchResultsView, &QListWidget::itemActivated, this, [this](QListWidgetItem * Item)
    {
      // TPreferencesDialog::SearchResultClick
      QWidget * Control = d->SearchResultControls.value(Item->data(Qt::UserRole).toInt());
      if (Control == nullptr)
      {
        return;
      }
      QWidget * Page = Control;
      while ((Page != nullptr) && (Page->parentWidget() != d->PageControl))
      {
        Page = Page->parentWidget();
      }
      int Index = d->PageControl->indexOf(Page);
      d->SearchEdit->blockSignals(true);
      d->SearchEdit->clear();
      d->SearchEdit->blockSignals(false);
      for (QTreeWidgetItemIterator It(d->NavigationTree); *It; ++It)
      {
        if ((*It)->data(0, Qt::UserRole).toInt() == Index)
        {
          d->NavigationTree->setCurrentItem(*It);
          d->PageControl->setCurrentIndex(Index);
        }
      }
      Control->setFocus();
    });
  }

  d->NavigationTree->expandAll();
  connect(d->NavigationTree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem * Item)
  {
    if (Item != nullptr)
    {
      d->PageControl->setCurrentIndex(Item->data(0, Qt::UserRole).toInt());
    }
  });

  auto * Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Help);
  connect(Buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  // TPreferencesDialog::WMHelp: Help and F1 open the help of the active page
  Buttons->button(QDialogButtonBox::Help)->setText(QStringLiteral("&Help"));
  HelpSystem::InstallFormHelp(this, QStringLiteral("ui_preferences"), Buttons->button(QDialogButtonBox::Help));
  d->SearchSheet->setProperty("PageHelpKeyword", QStringLiteral("ui_pref_search"));
  auto UpdateHelpKeyword = [this]()
  {
    QWidget * Page = d->PageControl->currentWidget();
    QString Keyword = (Page != nullptr) ? Page->property("PageHelpKeyword").toString() : QString();
    HelpSystem::SetFormHelpKeyword(this, Keyword.isEmpty() ? QStringLiteral("ui_preferences") : Keyword);
  };
  connect(d->PageControl, &QStackedWidget::currentChanged, this, UpdateHelpKeyword);
  UpdateHelpKeyword();
  d->SearchEdit = new QLineEdit();
  // the search box has its own help (SearchEdit HelpKeyword)
  d->SearchEdit->setProperty("HelpKeyword", QStringLiteral("ui_pref_search"));
  d->SearchEdit->setPlaceholderText(QStringLiteral("Search"));
  d->SearchEdit->setClearButtonEnabled(true);
  d->SearchEdit->setFixedWidth(d->NavigationTree->width());
  connect(d->SearchEdit, &QLineEdit::textChanged, this, [this](const QString & Text) { Search(Text); });
  auto * LeftPanel = new QVBoxLayout();
  LeftPanel->addWidget(d->SearchEdit);
  LeftPanel->addWidget(d->NavigationTree, 1);
  auto * Body = new QHBoxLayout();
  Body->addLayout(LeftPanel);
  Body->addWidget(d->PageControl, 1);
  auto * Layout = new QVBoxLayout(this);
  Layout->addLayout(Body, 1);
  Layout->addWidget(Buttons);

  auto Update = [this]() { UpdateControls(); };
  for (QCheckBox * C : findChildren<QCheckBox *>())
  {
    connect(C, &QCheckBox::toggled, this, Update);
  }
  for (QRadioButton * R : findChildren<QRadioButton *>())
  {
    connect(R, &QRadioButton::toggled, this, Update);
  }
  for (QComboBox * C : findChildren<QComboBox *>())
  {
    connect(C, &QComboBox::currentIndexChanged, this, Update);
  }

  LoadConfiguration();
  d->NavigationTree->setCurrentItem(d->NavigationTree->topLevelItem(0));
  // TPreferencesDialog::LoadConfiguration: ActiveControl = NavigationTree
  d->NavigationTree->setFocus();
}

PreferencesDialog::~PreferencesDialog()
{
  delete d;
}

bool PreferencesDialog::Execute(QWidget * Parent, const QString & InitialPage)
{
  PreferencesDialog Dialog(Parent);
  if (!InitialPage.isEmpty())
  {
    Dialog.SelectPage(InitialPage);
  }
  return (Dialog.exec() == QDialog::Accepted);
}

void PreferencesDialog::SelectPage(const QString & Caption)
{
  for (QTreeWidgetItemIterator It(d->NavigationTree); *It; ++It)
  {
    if (((*It)->data(0, Qt::UserRole + 1).toString().compare(Caption, Qt::CaseInsensitive) == 0) ||
        ((*It)->text(0).compare(Caption, Qt::CaseInsensitive) == 0))
    {
      d->NavigationTree->setCurrentItem(*It);
      return;
    }
  }
}

// TPreferencesDialog::Search: captions of the options containing Text
static QString StripHotkey(const QString & Caption)
{
  QString Result = Caption;
  Result.replace(QStringLiteral("&&"), QStringLiteral("\x01"));
  Result.remove(QLatin1Char('&'));
  Result.replace(QStringLiteral("\x01"), QStringLiteral("&"));
  return Result;
}

void PreferencesDialog::Search(const QString & Text)
{
  if (d->SearchEdit->text() != Text)
  {
    d->SearchEdit->setText(Text);   // calls back
    return;
  }
  d->SearchResultsView->clear();
  d->SearchResultControls.clear();
  if (Text.trimmed().isEmpty())
  {
    if (QTreeWidgetItem * Item = d->NavigationTree->currentItem())
    {
      d->PageControl->setCurrentIndex(Item->data(0, Qt::UserRole).toInt());
    }
    return;
  }
  // ContainsTextSemiCaseSensitive: case sensitive only when the text has upper case letters
  Qt::CaseSensitivity Sensitivity = (Text != Text.toLower()) ? Qt::CaseSensitive : Qt::CaseInsensitive;
  auto Add = [&](const QString & Caption, QWidget * Control, const QString & Page)
  {
    QString Plain = StripHotkey(Caption);
    // do not search unit labels (like "s")
    if ((Plain.size() >= 4) && Plain.contains(Text.trimmed(), Sensitivity))
    {
      auto * Item = new QListWidgetItem(Plain, d->SearchResultsView);
      Item->setToolTip(Page);
      Item->setData(Qt::UserRole, static_cast<int>(d->SearchResultControls.size()));
      d->SearchResultControls.append(Control);
    }
  };
  // the pages in the tree order
  for (QTreeWidgetItemIterator It(d->NavigationTree); *It; ++It)
  {
    QWidget * Page = d->PageControl->widget((*It)->data(0, Qt::UserRole).toInt());
    QString PageCaption = (*It)->text(0);
    Add(PageCaption, Page, PageCaption);
    for (QWidget * Control : Page->findChildren<QWidget *>())
    {
      if (Control->isHidden())
      {
        continue;
      }
      if (auto * Button = qobject_cast<QAbstractButton *>(Control))
      {
        Add(Button->text(), Control, PageCaption);
      }
      else if (auto * Label = qobject_cast<QLabel *>(Control))
      {
        for (const QString & Line : Label->text().split(QLatin1Char('\n')))
        {
          Add(Line, (Label->buddy() != nullptr) ? Label->buddy() : Control, PageCaption);
        }
      }
      else if (auto * Group = qobject_cast<QGroupBox *>(Control))
      {
        Add(Group->title(), Control, PageCaption);
      }
      else if (auto * Combo = qobject_cast<QComboBox *>(Control))
      {
        if (!Combo->isEditable())
        {
          for (int Index = 0; Index < Combo->count(); Index++)
          {
            Add(Combo->itemText(Index), Control, PageCaption);
          }
        }
      }
    }
  }
  d->PageControl->setCurrentWidget(d->SearchSheet);
}

void PreferencesDialog::LoadConfiguration()
{
  Configuration & C = Configuration::Instance();
  d->NoUpdate = true;

  d->CopyParams = C.DefaultCopyParam();

  // Environment
  d->ConfirmOverwritingCheck->setChecked(d->CopyParams.ConfirmOverwriting);
  d->ConfirmResumeCheck->setChecked(d->CopyParams.ConfirmResume);
  d->ConfirmDeletingCheck->setChecked(C.ConfirmDeleting);
  d->ConfirmRecyclingCheck->setChecked(C.ConfirmRecycling);
  d->ConfirmClosingSessionCheck->setChecked(C.ConfirmClosingSession);
  d->ConfirmExitOnCompletionCheck->setChecked(C.ConfirmExitOnCompletion);
  d->ConfirmCommandSessionCheck->setChecked(C.ConfirmCommandSession);
  d->ConfirmTransferringCheck->setChecked(C.ConfirmTransferring);
  d->ContinueOnErrorCheck->setChecked(C.ContinueOnError);
  d->SynchronizeSummaryCheck->setChecked(C.SynchronizeSummary);
  // allow grayed state only initially (as WinSCP)
  d->DDTransferConfirmationCheck->setTristate(C.DDTransferConfirmation == asAuto);
  d->DDTransferConfirmationCheck->setCheckState(
    (C.DDTransferConfirmation == asOn) ? Qt::Checked : ((C.DDTransferConfirmation == asOff) ? Qt::Unchecked : Qt::PartiallyChecked));
  d->BeepOnFinishCheck->setChecked(C.BeepOnFinish);
  d->BeepOnFinishAfterEdit->setValue(C.BeepOnFinishAfter);
  d->BalloonNotificationsCheck->setChecked(C.BalloonNotifications);
  d->FlashTaskbarCheck->setChecked(C.FlashTaskbar);

  // Interface: ComboAutoSwitchLoad with Automatic/Light/Dark
  d->ThemeCombo->setCurrentIndex((C.DarkTheme == asOn) ? 2 : ((C.DarkTheme == asOff) ? 1 : 0));

  // Window
  d->PathInCaptionFullButton->setChecked(C.PathInCaption == picFull);
  d->PathInCaptionShortButton->setChecked(C.PathInCaption == picShort);
  d->PathInCaptionNoneButton->setChecked(C.PathInCaption == picNone);
  d->MinimizeToTrayCheck->setChecked(C.MinimizeToTray);
  d->ExternalSessionInExistingInstanceCheck->setChecked(C.ExternalSessionInExistingInstance);
  d->ShowLoginWhenNoSessionCheck->setChecked(C.ShowLoginOnStartup);
  d->KeepOpenWhenNoSessionCheck->setChecked(C.KeepOpenWhenNoSession);
  d->SessionTabCaptionTruncationCheck->setChecked(C.SessionTabCaptionTruncation);
  d->SessionSilentDisconnectCheck->setChecked(C.SessionSilentDisconnect);
  d->AutoSaveWorkspaceCheck->setChecked(C.AutoSaveWorkspace);
  // It will rarely happen that LastWorkspace is set, while AutoWorkspace not
  d->AutoWorkspaceCombo->setEditText(!C.AutoWorkspace.isEmpty() ? C.AutoWorkspace :
    (!Workspaces::LastWorkspace().isEmpty() ? Workspaces::LastWorkspace() : Workspaces::NewWorkspaceName()));
  d->AutoSaveWorkspacePasswordsCheck->setChecked(C.AutoSaveWorkspacePasswords);
  d->WorkspaceConnectAllCheck->setChecked(C.WorkspaceConnectAll);

  // Commander
  d->NortonLikeModeCombo->setCurrentIndex((C.NortonLikeMode == nlOff) ? 2 : ((C.NortonLikeMode == nlKeyboard) ? 1 : 0));
  d->SwappedPanelsCheck->setChecked(C.SwappedPanels);
  d->TreeOnLeftCheck->setChecked(C.TreeOnLeft);
  d->ExplorerKeyboardShortcutsCombo->setCurrentIndex(C.ExplorerKeyboardShortcuts ? 1 : 0);
  d->UseLocationProfilesCheck->setChecked(C.UseLocationProfiles);
  d->CompareByTimeCheck->setChecked(C.CompareByTime);
  d->CompareBySizeCheck->setChecked(C.CompareBySize);

  // Panels
  d->ShowHiddenFilesCheck->setChecked(C.ShowHiddenFiles);
  d->DefaultDirIsHomeCheck->setChecked(C.DefaultDirIsHome);
  d->PreservePanelStateCheck->setChecked(C.PreservePanelState);
  d->RenameWholeNameCheck->setChecked(C.RenameWholeName);
  d->FullRowSelectCheck->setChecked(C.FullRowSelect);
  d->NaturalOrderNumericalSortingCheck->setChecked(C.NaturalOrderNumericalSorting);
  d->AlwaysSortDirectoriesByNameCheck->setChecked(C.AlwaysSortDirectoriesByName);
  d->FormatSizeBytesCombo->setCurrentIndex(qBound(0, static_cast<int>(C.FormatSizeBytes), 2));
  d->PanelSearchCombo->setCurrentIndex(qBound(-1, static_cast<int>(C.PanelSearch), 2));
  // The legacy flag may have been changed by GUI code since Load
  TDoubleClickAction DoubleClickAction = C.DoubleClickAction;
  if (C.CopyOnDoubleClick != (DoubleClickAction == dcaCopy))
  {
    DoubleClickAction = C.CopyOnDoubleClick ? dcaCopy : dcaOpen;
  }
  d->DoubleClickActionCombo->setCurrentIndex(qBound(0, static_cast<int>(DoubleClickAction), 2));
  d->CopyOnDoubleClickConfirmationCheck->setChecked(C.CopyOnDoubleClickConfirmation);
  d->PanelFontName = C.PanelFontName;
  d->PanelFontSize = C.PanelFontSize;
  d->PanelFontCheck->setChecked(!C.PanelFontName.isEmpty());

  // File colors (TFileColorData::LoadList)
  d->FileColors.clear();
  {
    IniStorage Storage(&C.Ini());
    QString Colors;
    if (Storage.OpenSubKeyPath(QStringLiteral("Configuration\\Interface"), false))
    {
      Colors = Storage.ReadString(QStringLiteral("FileColors"), QString());
    }
    for (const FileColorRule & Rule : FileColorRule::LoadList(Colors))
    {
      d->FileColors.append(qMakePair(Rule.Mask, Rule.Color.red() | (Rule.Color.green() << 8) | (Rule.Color.blue() << 16)));
    }
  }
  UpdateFileColorsView();

  // Remote / Local
  d->ShowInaccesibleDirectoriesCheck->setChecked(C.ShowInaccesibleDirectories);
  d->AutoReadDirectoryAfterOpCheck->setChecked(C.AutoReadDirectoryAfterOp);
  d->RefreshRemotePanelCheck->setChecked(C.RefreshRemotePanel);
  d->RefreshRemotePanelIntervalEdit->setValue(C.RefreshRemotePanelInterval);
  d->PreserveLocalDirectoryCheck->setChecked(C.PreserveLocalDirectory);
  d->SystemContextMenuCheck->setChecked(C.SystemContextMenu);
  d->DeleteToRecycleBinCheck->setChecked(C.DeleteToRecycleBin);

  // Editors
  d->EditorList = C.Editors;
  UpdateEditorListView();
  if (!d->EditorList.isEmpty())
  {
    d->EditorListView->setCurrentItem(d->EditorListView->topLevelItem(0));
  }
  d->EditorCheckNotModifiedCheck->setChecked(C.EditorCheckNotModified);
  d->EditorWordWrapCheck->setChecked(C.EditorWordWrap);
  d->EditorTabSizeEdit->setValue(C.EditorTabSize);
  d->EditorEncodingCombo->setCurrentIndex((C.EditorEncoding == EditorWindow::CodePageUTF8) ? 1 : 0);
  d->EditorFontColor = C.EditorFontColor;
  d->EditorBackgroundColor = C.EditorBackgroundColor;
  d->EditorFontName = C.EditorFontName;
  d->EditorFontSize = C.EditorFontSize;
  d->EditorWarnLargeFileCheck->setChecked(C.EditorWarnOrLargeFileSize);
  d->EditorLargeFileSizeEdit->setValue(C.EditorLargeFileSize);

  // Transfer
  d->CopyParamAutoSelectNoticeCheck->setChecked(C.CopyParamAutoSelectNotice);
  d->CopyParamPresetsFrame->SetList(CopyParamPresets::Instance().List());
  d->DDWarnLackOfTempSpaceCheck->setChecked(C.DDWarnLackOfTempSpace);
  d->DDSystemTemporaryDirectoryButton->setChecked(C.DDTemporaryDirectory.isEmpty());
  d->DDCustomTemporaryDirectoryButton->setChecked(!C.DDTemporaryDirectory.isEmpty());
  d->DDTemporaryDirectoryEdit->setText(C.DDTemporaryDirectory.isEmpty() ? QDir::tempPath() : C.DDTemporaryDirectory);

  // Background
  d->QueueTransferLimitEdit->setValue(C.QueueTransfersLimit);
  d->EnableQueueByDefaultCheck->setChecked(C.EnableQueueByDefault);
  d->QueueAutoPopupCheck->setChecked(C.QueueAutoPopup);
  d->QueueCheck->setChecked(d->CopyParams.Queue);
  d->QueueParallelCheck->setChecked(d->CopyParams.QueueParallel);
  d->QueueNoConfirmationCheck->setChecked(d->CopyParams.QueueNoConfirmation);
  d->ParallelTransferCheck->setChecked(C.ParallelTransferThreshold > 0);
  qint64 ParallelTransferThreshold = static_cast<qint64>(C.ParallelTransferThreshold) * 1024;
  d->ParallelTransferThresholdCombo->setCurrentText(SizeToStr((ParallelTransferThreshold > 0) ? ParallelTransferThreshold : 100 * 1024 * 1024));
  int KeepIndex;
  if (!C.QueueKeepDoneItems) KeepIndex = 0;
  else if (C.QueueKeepDoneItemsFor < 0) KeepIndex = 5;
  else if (C.QueueKeepDoneItemsFor <= 15) KeepIndex = 1;
  else if (C.QueueKeepDoneItemsFor <= 60) KeepIndex = 2;
  else if (C.QueueKeepDoneItemsFor <= 15 * 60) KeepIndex = 3;
  else KeepIndex = 4;
  d->QueueKeepDoneItemsForCombo->setCurrentIndex(KeepIndex);
  d->QueueViewShowButton->setChecked(C.QueueViewShow == qvShow);
  d->QueueViewHideWhenEmptyButton->setChecked(C.QueueViewShow == qvHideWhenEmpty);
  d->QueueViewHideButton->setChecked(C.QueueViewShow == qvHide);

  // Endurance
  d->ResumeOnButton->setChecked(d->CopyParams.ResumeSupport == CopyParam::rsOn);
  d->ResumeSmartButton->setChecked(d->CopyParams.ResumeSupport == CopyParam::rsSmart);
  d->ResumeOffButton->setChecked(d->CopyParams.ResumeSupport == CopyParam::rsOff);
  d->ResumeThresholdEdit->setValue(static_cast<int>(d->CopyParams.ResumeThreshold / 1024));
  d->SessionReopenAutoCheck->setChecked(C.SessionReopenAuto > 0);
  d->SessionReopenAutoEdit->setValue((C.SessionReopenAuto > 0) ? (C.SessionReopenAuto / 1000) : 5);
  d->SessionReopenAutoIdleCheck->setChecked(C.SessionReopenAutoIdleOn && (C.SessionReopenAutoIdle > 0));
  d->SessionReopenAutoIdleEdit->setValue(((C.SessionReopenAutoIdle > 0) ? C.SessionReopenAutoIdle : 9000) / 1000);
  d->SessionReopenAutoInactiveCheck->setChecked(C.SessionReopenAutoInactive);
  d->SessionReopenAutoStallCheck->setChecked(C.SessionReopenAutoStall > 0);
  d->SessionReopenAutoStallEdit->setValue((C.SessionReopenAutoStall > 0) ? (C.SessionReopenAutoStall / 1000) : 60);
  d->SessionReopenTimeoutEdit->setValue(C.SessionReopenTimeout / 1000);

  // Network
  d->RetrieveExternalIpAddressButton->setChecked(C.ExternalIpAddress.isEmpty());
  d->CustomExternalIpAddressButton->setChecked(!C.ExternalIpAddress.isEmpty());
  d->CustomExternalIpAddressEdit->setText(C.ExternalIpAddress);
  d->LocalPortNumberCheck->setChecked((C.LocalPortNumberMin > 0) && (C.LocalPortNumberMax >= C.LocalPortNumberMin));
  d->LocalPortNumberMinEdit->setValue((C.LocalPortNumberMin > 0) ? C.LocalPortNumberMin : 49152);
  d->LocalPortNumberMaxEdit->setValue(
    (C.LocalPortNumberMax >= d->LocalPortNumberMinEdit->value()) ? C.LocalPortNumberMax : 65535);
  d->TryFtpWhenSshFailsCheck->setChecked(C.TryFtpWhenSshFails);

  // Languages
  LoadLanguages();

  // Security
  d->UseMasterPasswordCheck->setChecked(C.UseMasterPassword());
  d->SessionRememberPasswordCheck->setChecked(C.SessionRememberPassword);

  // Storage
  bool ExplicitStorage = !C.ExplicitIniFileStorageName().isEmpty();
  d->AutomaticIniFileStorageButton->setChecked(!ExplicitStorage && C.CustomIniFileStorageName().isEmpty());
  d->CustomIniFileStorageButton->setChecked(!ExplicitStorage && !C.CustomIniFileStorageName().isEmpty());
  d->CustomIniFileStorageEdit->setText(
    C.CustomIniFileStorageName().isEmpty() ? C.DefaultIniFileExportPath() : C.CustomIniFileStorageName());
  d->StorageGroup->setEnabled(!C.IsNulStorage());
  d->CheckForUpdatesCheck->setChecked(C.CheckForUpdates);
  d->UpdateLastCheckLabel->setText(C.UpdateLastCheck.isValid() ?
    QStringLiteral("Last check: %1").arg(QLocale().toString(C.UpdateLastCheck.toLocalTime(), QLocale::ShortFormat)) :
    QStringLiteral("Not checked yet."));
  d->TemporaryDirectoryCleanupCheck->setChecked(C.TemporaryDirectoryCleanup);
  d->ConfirmTemporaryDirectoryCleanupCheck->setChecked(C.ConfirmTemporaryDirectoryCleanup);
  d->TemporaryDirectoryAppendSessionCheck->setChecked(C.TemporaryDirectoryAppendSession);
  d->TemporaryDirectoryAppendPathCheck->setChecked(C.TemporaryDirectoryAppendPath);
  d->TemporaryDirectoryDeterministicCheck->setChecked(C.TemporaryDirectoryDeterministic);

  // Logging
  d->EnableLoggingCheck->setChecked(C.Logging);
  d->LogProtocolCombo->setCurrentIndex(qBound(0, C.LogProtocol + 1, 3));
  d->LogFileNameEdit->setText(!C.LogFileName.isEmpty() ? C.LogFileName : Configuration::DefaultLogFileName());
  d->LogFileAppendButton->setChecked(C.LogFileAppend);
  d->LogFileOverwriteButton->setChecked(!C.LogFileAppend);
  d->LogMaxSizeCheck->setChecked(C.LogMaxSize > 0);
  d->LogMaxSizeCombo->setCurrentText(SizeToStr((C.LogMaxSize > 0) ? C.LogMaxSize : (1 * 1024 * 1024)));
  d->LogMaxSizeCountCheck->setChecked(C.LogMaxCount > 0);
  d->LogMaxSizeCountEdit->setValue((C.LogMaxCount > 0) ? C.LogMaxCount : 5);
  d->LogSensitiveCheck->setChecked(C.LogSensitive);
  d->EnableActionsLoggingCheck->setChecked(C.LogActions);
  d->ActionsLogFileNameEdit->setText(!C.ActionsLogFileName.isEmpty() ? C.ActionsLogFileName : SessionLog::DefaultActionsLogFileName());

  // Applications
  d->PuttyPathEdit->setText(C.PuttyPath);
  d->PuttyPasswordCheck->setChecked(C.PuttyPassword);
  d->AutoOpenInPuttyCheck->setChecked(C.AutoOpenInPutty);
  d->TelnetForFtpInPuttyCheck->setChecked(C.TelnetForFtpInPutty);

  d->NoUpdate = false;
  UpdateControls();
}

void PreferencesDialog::SaveConfiguration()
{
  Configuration & C = Configuration::Instance();

  // Environment
  d->CopyParams.ConfirmOverwriting = d->ConfirmOverwritingCheck->isChecked();
  d->CopyParams.ConfirmResume = d->ConfirmResumeCheck->isChecked();
  C.ConfirmDeleting = d->ConfirmDeletingCheck->isChecked();
  C.ConfirmRecycling = d->ConfirmRecyclingCheck->isChecked();
  C.ConfirmClosingSession = d->ConfirmClosingSessionCheck->isChecked();
  C.ConfirmExitOnCompletion = d->ConfirmExitOnCompletionCheck->isChecked();
  C.ConfirmCommandSession = d->ConfirmCommandSessionCheck->isChecked();
  C.ConfirmTransferring = d->ConfirmTransferringCheck->isChecked();
  C.ContinueOnError = d->ContinueOnErrorCheck->isChecked();
  C.SynchronizeSummary = d->SynchronizeSummaryCheck->isChecked();
  switch (d->DDTransferConfirmationCheck->checkState())
  {
    case Qt::Checked: C.DDTransferConfirmation = asOn; break;
    case Qt::Unchecked: C.DDTransferConfirmation = asOff; break;
    default: C.DDTransferConfirmation = asAuto; break;
  }
  C.BeepOnFinish = d->BeepOnFinishCheck->isChecked();
  C.BeepOnFinishAfter = d->BeepOnFinishAfterEdit->value();
  C.BalloonNotifications = d->BalloonNotificationsCheck->isChecked();
  C.FlashTaskbar = d->FlashTaskbarCheck->isChecked();

  // Interface
  static const TAutoSwitch Themes[] = { asAuto, asOff, asOn };
  C.DarkTheme = Themes[qBound(0, d->ThemeCombo->currentIndex(), 2)];

  // Window
  C.PathInCaption = d->PathInCaptionFullButton->isChecked() ? picFull :
    (d->PathInCaptionShortButton->isChecked() ? picShort : picNone);
  C.MinimizeToTray = d->MinimizeToTrayCheck->isChecked();
  C.ExternalSessionInExistingInstance = d->ExternalSessionInExistingInstanceCheck->isChecked();
  C.ShowLoginOnStartup = d->ShowLoginWhenNoSessionCheck->isChecked();
  C.KeepOpenWhenNoSession = d->KeepOpenWhenNoSessionCheck->isChecked();
  C.SessionTabCaptionTruncation = d->SessionTabCaptionTruncationCheck->isChecked();
  C.SessionSilentDisconnect = d->SessionSilentDisconnectCheck->isChecked();
  QString AutoWorkspace = d->AutoWorkspaceCombo->currentText().trimmed();
  C.AutoSaveWorkspace = !AutoWorkspace.isEmpty() && d->AutoSaveWorkspaceCheck->isChecked();
  if (!AutoWorkspace.isEmpty())
  {
    C.AutoWorkspace = AutoWorkspace;
  }
  C.AutoSaveWorkspacePasswords = d->AutoSaveWorkspacePasswordsCheck->isChecked();
  C.WorkspaceConnectAll = d->WorkspaceConnectAllCheck->isChecked();

  // Commander
  static const TNortonLikeMode NortonLikeModes[] = { nlOn, nlKeyboard, nlOff };
  C.NortonLikeMode = NortonLikeModes[qBound(0, d->NortonLikeModeCombo->currentIndex(), 2)];
  C.SwappedPanels = d->SwappedPanelsCheck->isChecked();
  C.TreeOnLeft = d->TreeOnLeftCheck->isChecked();
  C.ExplorerKeyboardShortcuts = (d->ExplorerKeyboardShortcutsCombo->currentIndex() == 1);
  C.UseLocationProfiles = d->UseLocationProfilesCheck->isChecked();
  C.CompareByTime = d->CompareByTimeCheck->isChecked();
  C.CompareBySize = d->CompareBySizeCheck->isChecked();

  // Panels
  C.ShowHiddenFiles = d->ShowHiddenFilesCheck->isChecked();
  C.DefaultDirIsHome = d->DefaultDirIsHomeCheck->isChecked();
  C.PreservePanelState = d->PreservePanelStateCheck->isChecked();
  C.RenameWholeName = d->RenameWholeNameCheck->isChecked();
  C.FullRowSelect = d->FullRowSelectCheck->isChecked();
  C.NaturalOrderNumericalSorting = d->NaturalOrderNumericalSortingCheck->isChecked();
  C.AlwaysSortDirectoriesByName = d->AlwaysSortDirectoriesByNameCheck->isChecked();
  C.FormatSizeBytes = static_cast<TFormatBytesStyle>(d->FormatSizeBytesCombo->currentIndex());
  C.PanelSearch = static_cast<TIncrementalSearch>(d->PanelSearchCombo->currentIndex());
  C.DoubleClickAction = static_cast<TDoubleClickAction>(d->DoubleClickActionCombo->currentIndex());
  C.CopyOnDoubleClick = (C.DoubleClickAction == dcaCopy);
  C.CopyOnDoubleClickConfirmation = d->CopyOnDoubleClickConfirmationCheck->isChecked();
  // StoreFont(FPanelFont): checking "Use custom font" alone keeps the font shown
  // in the label (the current panel font)
  C.PanelFontName = d->PanelFontCheck->isChecked() ? (d->PanelFontName.isEmpty() ? font().family() : d->PanelFontName) : QString();
  C.PanelFontSize = d->PanelFontCheck->isChecked() ? ((d->PanelFontSize > 0) ? d->PanelFontSize : font().pointSize()) : 0;

  // File colors (TFileColorData::SaveList), kept raw as FilePanel reads them
  {
    QList<FileColorRule> Rules;
    for (const auto & Item : d->FileColors)
    {
      FileColorRule Rule;
      Rule.Mask = Item.first;
      Rule.Color = QColor(Item.second & 0xFF, (Item.second >> 8) & 0xFF, (Item.second >> 16) & 0xFF);
      Rules.append(Rule);
    }
    QString Value = FileColorRule::SaveList(Rules);
    IniStorage Storage(&C.Ini());
    bool Exists = Storage.OpenSubKeyPath(QStringLiteral("Configuration\\Interface"), false);
    QString Stored = Exists ? Storage.ReadString(QStringLiteral("FileColors"), QString()) : QString();
    if (Exists)
    {
      Storage.CloseSubKeyPath();
    }
    // written only when changed, the keys of the other instances stay untouched
    if ((Value != Stored) && Storage.OpenSubKeyPath(QStringLiteral("Configuration\\Interface"), true))
    {
      Storage.WriteString(QStringLiteral("FileColors"), Value);
    }
  }

  // Remote / Local
  C.ShowInaccesibleDirectories = d->ShowInaccesibleDirectoriesCheck->isChecked();
  C.AutoReadDirectoryAfterOp = d->AutoReadDirectoryAfterOpCheck->isChecked();
  C.RefreshRemotePanel = d->RefreshRemotePanelCheck->isChecked();
  C.RefreshRemotePanelInterval = d->RefreshRemotePanelIntervalEdit->value();
  C.PreserveLocalDirectory = d->PreserveLocalDirectoryCheck->isChecked();
  C.SystemContextMenu = d->SystemContextMenuCheck->isChecked();
  C.DeleteToRecycleBin = d->DeleteToRecycleBinCheck->isChecked();

  // Editors
  C.Editors = d->EditorList.isEmpty() ? Configuration::DefaultEditors() : d->EditorList;
  C.EditorCheckNotModified = d->EditorCheckNotModifiedCheck->isChecked();
  C.EditorWordWrap = d->EditorWordWrapCheck->isChecked();
  C.EditorTabSize = d->EditorTabSizeEdit->value();
  C.EditorEncoding = (d->EditorEncodingCombo->currentIndex() == 1) ? EditorWindow::CodePageUTF8 : EditorWindow::CodePageSystem;
  C.EditorFontColor = d->EditorFontColor;
  C.EditorBackgroundColor = d->EditorBackgroundColor;
  C.EditorFontName = d->EditorFontName;
  C.EditorFontSize = d->EditorFontSize;
  C.EditorWarnOrLargeFileSize = d->EditorWarnLargeFileCheck->isChecked();
  C.EditorLargeFileSize = d->EditorLargeFileSizeEdit->value();

  // Transfer
  C.CopyParamAutoSelectNotice = d->CopyParamAutoSelectNoticeCheck->isChecked();
  C.DDWarnLackOfTempSpace = d->DDWarnLackOfTempSpaceCheck->isChecked();
  C.DDTemporaryDirectory = d->DDCustomTemporaryDirectoryButton->isChecked() ? d->DDTemporaryDirectoryEdit->text().trimmed() : QString();

  // Background
  C.QueueTransfersLimit = d->QueueTransferLimitEdit->value();
  C.EnableQueueByDefault = d->EnableQueueByDefaultCheck->isChecked();
  C.QueueAutoPopup = d->QueueAutoPopupCheck->isChecked();
  d->CopyParams.Queue = d->QueueCheck->isChecked();
  d->CopyParams.QueueParallel = d->QueueParallelCheck->isChecked();
  d->CopyParams.QueueNoConfirmation = d->QueueNoConfirmationCheck->isChecked();
  qint64 Threshold = 0;
  if (d->ParallelTransferCheck->isChecked() && TryStrToSize(d->ParallelTransferThresholdCombo->currentText(), Threshold))
  {
    C.ParallelTransferThreshold = static_cast<int>(qMin(Threshold / 1024, static_cast<qint64>(std::numeric_limits<int>::max())));
  }
  else
  {
    C.ParallelTransferThreshold = -1;
  }
  C.QueueKeepDoneItems = (d->QueueKeepDoneItemsForCombo->currentIndex() != 0);
  static const int KeepFor[] = { 0, 15, 60, 15 * 60, 60 * 60, -1 };
  if (C.QueueKeepDoneItems)
  {
    C.QueueKeepDoneItemsFor = KeepFor[qBound(0, d->QueueKeepDoneItemsForCombo->currentIndex(), 5)];
  }
  C.QueueViewShow = d->QueueViewShowButton->isChecked() ? qvShow :
    (d->QueueViewHideWhenEmptyButton->isChecked() ? qvHideWhenEmpty : qvHide);

  // Endurance
  d->CopyParams.ResumeSupport = d->ResumeOnButton->isChecked() ? CopyParam::rsOn :
    (d->ResumeSmartButton->isChecked() ? CopyParam::rsSmart : CopyParam::rsOff);
  d->CopyParams.ResumeThreshold = static_cast<qint64>(d->ResumeThresholdEdit->value()) * 1024;
  C.SessionReopenAuto = d->SessionReopenAutoCheck->isChecked() ? (d->SessionReopenAutoEdit->value() * 1000) : 0;
  C.SessionReopenAutoIdleOn = d->SessionReopenAutoIdleCheck->isChecked();
  C.SessionReopenAutoIdle = d->SessionReopenAutoIdleEdit->value() * 1000;
  C.SessionReopenAutoInactive = d->SessionReopenAutoInactiveCheck->isChecked();
  C.SessionReopenAutoStall = d->SessionReopenAutoStallCheck->isChecked() ? (d->SessionReopenAutoStallEdit->value() * 1000) : 0;
  C.SessionReopenTimeout = d->SessionReopenTimeoutEdit->value() * 1000;

  // Network
  C.ExternalIpAddress = d->CustomExternalIpAddressButton->isChecked() ? d->CustomExternalIpAddressEdit->text().trimmed() : QString();
  if (d->LocalPortNumberCheck->isChecked())
  {
    C.LocalPortNumberMin = d->LocalPortNumberMinEdit->value();
    C.LocalPortNumberMax = d->LocalPortNumberMaxEdit->value();
  }
  else
  {
    C.LocalPortNumberMin = 0;
    C.LocalPortNumberMax = 0;
  }
  C.TryFtpWhenSshFails = d->TryFtpWhenSshFailsCheck->isChecked();

  // Languages
  if (QListWidgetItem * Item = d->LanguagesView->currentItem())
  {
    // do not change the locale, unless changed explicitly by user, to allow an
    // automatic switch to a new translation, once it becomes available
    unsigned int Lcid = Item->data(Qt::UserRole).toUInt();
    C.Locale = (Lcid == Translator::DefaultLcid()) ? 0 : static_cast<int>(Lcid);
  }

  // Security
  C.SessionRememberPassword = d->SessionRememberPasswordCheck->isChecked();

  // Storage (the configuration file itself is switched by accept)
  C.CheckForUpdates = d->CheckForUpdatesCheck->isChecked();
  C.TemporaryDirectoryCleanup = d->TemporaryDirectoryCleanupCheck->isChecked();
  C.ConfirmTemporaryDirectoryCleanup = d->ConfirmTemporaryDirectoryCleanupCheck->isChecked();
  C.TemporaryDirectoryAppendSession = d->TemporaryDirectoryAppendSessionCheck->isChecked();
  C.TemporaryDirectoryAppendPath = d->TemporaryDirectoryAppendPathCheck->isChecked();
  C.TemporaryDirectoryDeterministic = d->TemporaryDirectoryDeterministicCheck->isChecked();

  // Logging
  C.Logging = d->EnableLoggingCheck->isChecked();
  C.LogProtocol = d->LogProtocolCombo->currentIndex() - 1;
  C.LogFileName = d->LogFileNameEdit->text().trimmed();
  C.LogFileAppend = d->LogFileAppendButton->isChecked();
  qint64 LogMaxSize = 0;
  if (d->LogMaxSizeCheck->isChecked() && TryStrToSize(d->LogMaxSizeCombo->currentText(), LogMaxSize))
  {
    C.LogMaxSize = LogMaxSize;
  }
  else
  {
    C.LogMaxSize = 0;
  }
  C.LogMaxCount = d->LogMaxSizeCountCheck->isChecked() ? d->LogMaxSizeCountEdit->value() : 0;
  C.LogSensitive = d->LogSensitiveCheck->isChecked();
  C.LogActions = d->EnableActionsLoggingCheck->isChecked();
  C.ActionsLogFileName = d->ActionsLogFileNameEdit->text().trimmed();
  // the values confirmed in Preferences are permanent (also those set by /log & co.)
  SessionLog::ClearTemporaryLogging();

  // Applications
  C.PuttyPath = d->PuttyPathEdit->text().trimmed();
  C.PuttyPassword = d->PuttyPasswordCheck->isChecked();
  C.AutoOpenInPutty = d->AutoOpenInPuttyCheck->isChecked();
  C.TelnetForFtpInPutty = d->TelnetForFtpInPuttyCheck->isChecked();

  // before the default settings change, so that unchanged built-in presets stay built-in
  CopyParamPresets::Instance().SetList(d->CopyParamPresetsFrame->List());
  C.DefaultCopyParam() = d->CopyParams;
}

void PreferencesDialog::UpdateControls()
{
  if (d->NoUpdate)
  {
    return;
  }
  d->NoUpdate = true;

  // TPreferencesDialog::UpdateControls
  d->DDTransferConfirmationCheck->setEnabled(d->ConfirmTransferringCheck->isChecked());
  d->BeepOnFinishAfterEdit->setEnabled(d->BeepOnFinishCheck->isChecked());
  d->AutoSaveWorkspacePasswordsCheck->setEnabled(
    !Configuration::Instance().DisablePasswordStoring() && d->AutoSaveWorkspaceCheck->isChecked());
  d->AutoWorkspaceCombo->setEnabled(d->AutoSaveWorkspaceCheck->isChecked());
  d->CopyOnDoubleClickConfirmationCheck->setEnabled(d->DoubleClickActionCombo->currentIndex() == dcaCopy);
  d->PanelFontButton->setEnabled(d->PanelFontCheck->isChecked());
  d->PanelFontLabel->setEnabled(d->PanelFontCheck->isChecked());
  QString PanelFont = d->PanelFontName.isEmpty() ? font().family() : d->PanelFontName;
  int PanelFontSize = (d->PanelFontSize > 0) ? d->PanelFontSize : font().pointSize();
  d->PanelFontLabel->setText(QStringLiteral("%1, %2 pt").arg(PanelFont).arg(PanelFontSize));
  d->RefreshRemotePanelIntervalEdit->setEnabled(d->RefreshRemotePanelCheck->isChecked());

  int FileColorIndex = d->FileColorsView->indexOfTopLevelItem(d->FileColorsView->currentItem());
  d->EditFileColorButton->setEnabled(FileColorIndex >= 0);
  d->RemoveFileColorButton->setEnabled(FileColorIndex >= 0);
  d->UpFileColorButton->setEnabled(FileColorIndex > 0);
  d->DownFileColorButton->setEnabled((FileColorIndex >= 0) && (FileColorIndex < d->FileColors.size() - 1));

  int EditorIndex = d->EditorListView->indexOfTopLevelItem(d->EditorListView->currentItem());
  d->EditEditorButton->setEnabled(EditorIndex >= 0);
  d->RemoveEditorButton->setEnabled(EditorIndex >= 0);
  d->UpEditorButton->setEnabled(EditorIndex > 0);
  d->DownEditorButton->setEnabled((EditorIndex >= 0) && (EditorIndex < d->EditorList.size() - 1));
  QString EditorFont = d->EditorFontName.isEmpty() ? QFontDatabase::systemFont(QFontDatabase::FixedFont).family() : d->EditorFontName;
  int EditorFontSize = (d->EditorFontSize > 0) ? d->EditorFontSize : QFontDatabase::systemFont(QFontDatabase::FixedFont).pointSize();
  d->EditorFontLabel->setText(QStringLiteral("%1, %2 pt").arg(EditorFont).arg(EditorFontSize));
  d->EditorFontLabel->setFont(QFont(EditorFont, EditorFontSize));
  {
    QPalette Palette = d->EditorFontLabel->parentWidget()->palette();
    if (d->EditorBackgroundColor > 0)
    {
      int Color = d->EditorBackgroundColor;
      Palette.setColor(QPalette::Window, QColor(Color & 0xFF, (Color >> 8) & 0xFF, (Color >> 16) & 0xFF));
    }
    else
    {
      Palette.setColor(QPalette::Window, Palette.color(QPalette::Base));
    }
    if (d->EditorFontColor > 0)
    {
      int Color = d->EditorFontColor;
      Palette.setColor(QPalette::WindowText, QColor(Color & 0xFF, (Color >> 8) & 0xFF, (Color >> 16) & 0xFF));
    }
    else
    {
      Palette.setColor(QPalette::WindowText, Palette.color(QPalette::Text));
    }
    d->EditorFontLabel->setPalette(Palette);
  }
  d->EditorLargeFileSizeEdit->setEnabled(d->EditorWarnLargeFileCheck->isChecked());

  d->CopyParamPresetsFrame->UpdateControls();
  d->DDTemporaryDirectoryEdit->setEnabled(d->DDCustomTemporaryDirectoryButton->isChecked());

  d->ParallelTransferCheck->setEnabled(d->QueueParallelCheck->isChecked());
  d->ParallelTransferThresholdCombo->setEnabled(d->ParallelTransferCheck->isEnabled() && d->ParallelTransferCheck->isChecked());

  d->ResumeThresholdEdit->setEnabled(d->ResumeSmartButton->isChecked());
  d->SessionReopenAutoEdit->setEnabled(d->SessionReopenAutoCheck->isChecked());
  d->SessionReopenAutoIdleEdit->setEnabled(d->SessionReopenAutoIdleCheck->isChecked());
  d->SessionReopenAutoStallEdit->setEnabled(d->SessionReopenAutoStallCheck->isChecked());
  d->SessionReopenTimeoutEdit->setEnabled(
    d->SessionReopenAutoEdit->isEnabled() || d->SessionReopenAutoIdleCheck->isChecked() || d->SessionReopenAutoStallCheck->isChecked());

  d->CustomExternalIpAddressEdit->setEnabled(d->CustomExternalIpAddressButton->isChecked());
  d->LocalPortNumberMinEdit->setEnabled(d->LocalPortNumberCheck->isChecked());
  d->LocalPortNumberMaxEdit->setEnabled(d->LocalPortNumberCheck->isChecked());

  bool Logging = d->EnableLoggingCheck->isChecked();
  d->LogProtocolCombo->setEnabled(Logging);
  d->LogFileNameEdit->setEnabled(Logging);
  d->LogFileNameBrowse->setEnabled(Logging);
  d->LogFileNameHintText->setEnabled(Logging);
  d->LogFileAppendButton->setEnabled(Logging);
  d->LogFileOverwriteButton->setEnabled(Logging);
  d->LogMaxSizeCheck->setEnabled(Logging);
  d->LogMaxSizeCombo->setEnabled(Logging && d->LogMaxSizeCheck->isChecked());
  d->LogMaxSizeCountCheck->setEnabled(d->LogMaxSizeCombo->isEnabled());
  d->LogMaxSizeCountEdit->setEnabled(d->LogMaxSizeCountCheck->isEnabled() && d->LogMaxSizeCountCheck->isChecked());
  d->LogMaxSizeCountFilesLabel->setEnabled(d->LogMaxSizeCountEdit->isEnabled());
  d->LogSensitiveCheck->setEnabled(Logging);
  // LogProtocolHintLabel->Visible = (LogProtocolCombo2->ItemIndex - BelowNormalLogLevels >= 2)
  d->LogProtocolHintLabel->setVisible(d->LogProtocolCombo->currentIndex() >= 2);
  d->ActionsLogFileNameEdit->setEnabled(d->EnableActionsLoggingCheck->isChecked());
  d->ActionsLogFileNameBrowse->setEnabled(d->ActionsLogFileNameEdit->isEnabled());
  d->ActionsLogFileNameHintText->setEnabled(d->ActionsLogFileNameEdit->isEnabled());

  // Languages
  QListWidgetItem * LanguageItem = d->LanguagesView->currentItem();
  // NixSCP applies a language change at once (Translator retranslates all
  // windows), so "Changes will apply on the next start." is never true
  Q_UNUSED(LanguageItem);
  d->LanguageChangeLabel->setVisible(false);
  // Security
  bool Vault = ConfigVault::Enabled();
  d->SetMasterPasswordButton->setEnabled(Security::UseMasterPassword() && !Vault);
  d->UseMasterPasswordCheck->setEnabled(!Vault || Security::UseMasterPassword());
  d->VaultLabel->setVisible(Vault);
  // Storage
  d->CustomIniFileStorageEdit->setEnabled(d->CustomIniFileStorageButton->isChecked());
  d->CustomIniFileStorageBrowse->setEnabled(d->CustomIniFileStorageButton->isChecked());
  d->ConfirmTemporaryDirectoryCleanupCheck->setEnabled(d->TemporaryDirectoryCleanupCheck->isChecked());

  d->NoUpdate = false;
}

// TPreferencesDialog::UpdateFileColorsView / FileColorsViewCustomDrawItem
void PreferencesDialog::UpdateFileColorsView()
{
  d->FileColorsView->clear();
  for (const auto & Item : d->FileColors)
  {
    auto * ViewItem = new QTreeWidgetItem(d->FileColorsView);
    ViewItem->setText(0, Item.first);
    if (Item.second > 0)
    {
      ViewItem->setForeground(0, QColor(Item.second & 0xFF, (Item.second >> 8) & 0xFF, (Item.second >> 16) & 0xFF));
    }
  }
}

void PreferencesDialog::UpdateEditorListView()
{
  d->EditorListView->clear();
  for (const EditorPreferences & E : d->EditorList)
  {
    auto * Item = new QTreeWidgetItem(d->EditorListView);
    Item->setText(0, E.Name());
    Item->setText(1, E.FileMask);
    Item->setText(2, ((E.Editor == edExternal) && E.ExternalEditorText) ? QStringLiteral("Yes") : QString());
  }
  UpdateControls();
}

bool PreferencesDialog::EditEditor(int Index)
{
  bool Add = (Index < 0);
  EditorPreferences Editor = Add ? EditorPreferences() : d->EditorList.value(Index);
  if (Add)
  {
    Editor.Editor = edExternal;
  }
  EditorPreferencesDialog Dialog(Editor, Add, this);
  if (Dialog.exec() != QDialog::Accepted)
  {
    return false;
  }
  if (Add)
  {
    // TPreferencesDialog::AddEditEditorButtonClick: new editor goes before the selected one
    int Current = d->EditorListView->indexOfTopLevelItem(d->EditorListView->currentItem());
    Index = (Current >= 0) ? Current : d->EditorList.size();
    d->EditorList.insert(Index, Editor);
  }
  else
  {
    d->EditorList[Index] = Editor;
  }
  UpdateEditorListView();
  d->EditorListView->setCurrentItem(d->EditorListView->topLevelItem(Index));
  return true;
}

#ifndef Q_OS_WIN
// Quoting of an Exec key argument (Desktop Entry Specification, "The Exec key")
static QString UrlHandlerExecQuote(const QString & Arg)
{
  static const QString Reserved = QStringLiteral(" \t\n\"'\\><~|&;$*?#()`");
  bool NeedsQuoting = Arg.isEmpty();
  for (QChar C : Arg)
  {
    if (Reserved.contains(C))
    {
      NeedsQuoting = true;
      break;
    }
  }
  QString Result = Arg;
  // "%" is a field code everywhere
  Result.replace(QLatin1Char('%'), QStringLiteral("%%"));
  if (!NeedsQuoting)
  {
    return Result;
  }
  QString Quoted;
  for (QChar C : Result)
  {
    if ((C == QLatin1Char('"')) || (C == QLatin1Char('`')) || (C == QLatin1Char('$')) || (C == QLatin1Char('\\')))
    {
      Quoted += QLatin1Char('\\');
    }
    Quoted += C;
  }
  // the desktop entry string escaping doubles the backslashes once more
  Quoted.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
  return QLatin1Char('"') + Quoted + QLatin1Char('"');
}

// Removes the associations of the URL handler desktop file from a
// mimeapps.list (written by xdg-mime default); returns false on a write error
static bool RemoveUrlHandlerAssociations(const QString & FileName)
{
  QFile File(FileName);
  if (!File.exists())
  {
    return true;
  }
  if (!File.open(QIODevice::ReadOnly | QIODevice::Text))
  {
    return false;
  }
  const QStringList Lines = QString::fromUtf8(File.readAll()).split(QLatin1Char('\n'));
  File.close();
  QStringList Result;
  bool Changed = false;
  for (const QString & Line : Lines)
  {
    int Eq = Line.indexOf(QLatin1Char('='));
    QString Key = (Eq > 0) ? Line.left(Eq).trimmed() : QString();
    if (!Key.startsWith(QStringLiteral("x-scheme-handler/")) || Line.trimmed().startsWith(QLatin1Char('#')))
    {
      Result.append(Line);
      continue;
    }
    QStringList Apps = Line.mid(Eq + 1).split(QLatin1Char(';'), Qt::SkipEmptyParts);
    int Before = Apps.size();
    Apps.removeAll(UrlHandlerDesktopFile);
    if (Apps.size() == Before)
    {
      Result.append(Line);
      continue;
    }
    Changed = true;
    if (!Apps.isEmpty())
    {
      Result.append(Key + QLatin1Char('=') + Apps.join(QLatin1Char(';')) + QLatin1Char(';'));
    }
  }
  if (!Changed)
  {
    return true;
  }
  if (!File.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
  {
    return false;
  }
  File.write(Result.join(QLatin1Char('\n')).toUtf8());
  return true;
}
#endif

void PreferencesDialog::RegisterAsUrlHandler(bool Register)
{
  // RegisterAsUrlHandlerItemClick / UnregisterForDefaultProtocolsItemClick:
  // RegisterForDefaultProtocols / UnregisterForProtocols (windows/Setup.cpp),
  // done the freedesktop.org way: a desktop entry with x-scheme-handler MIME
  // types, made the default handler of the schemes with xdg-mime
  // (the per-user equivalent of the HKCU registration of WinSCP)
  QString Dir = QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation);
  QString FileName = Dir + QLatin1Char('/') + UrlHandlerDesktopFile;
  QStringList MimeTypes;
  for (const QString & Scheme : UrlSchemes)
  {
    MimeTypes.append(QStringLiteral("x-scheme-handler/") + Scheme);
  }
  QMessageBox Box(QMessageBox::Question, windowTitle(),
    // CONFIRM_REGISTER_URL2 / CONFIRM_UNREGISTER_URL
    Register ? QStringLiteral("Do you want to register NixSCP to handle URL addresses?") :
      QStringLiteral("Do you want to unregister NixSCP from handling all URL addresses?"),
    QMessageBox::Yes | QMessageBox::No, this);
  Box.setObjectName(QStringLiteral("RegisterUrlConfirmation"));
  Box.setInformativeText(Register ?
#ifdef Q_OS_WIN
    QStringLiteral("NixSCP will become the default application for %1 addresses of your user account.")
      .arg(UrlSchemes.join(QStringLiteral(", "))) :
#else
    QStringLiteral("NixSCP will become the default application for %1 addresses of your user account (%2).")
      .arg(UrlSchemes.join(QStringLiteral(", ")), FileName) :
#endif
    QStringLiteral("The association of %1 addresses with NixSCP will be removed.").arg(UrlSchemes.join(QStringLiteral(", "))));
  if (Box.exec() != QMessageBox::Yes)
  {
    return;
  }

  QString Error;
  QApplication::setOverrideCursor(Qt::WaitCursor);   // TInstantOperationVisualizer
#ifdef Q_OS_WIN
  // HKCU\Software\Classes, as WinSCP
  if (Register)
  {
    GuiCommandLine::RegisterForDefaultProtocols(&Error);
  }
  else
  {
    GuiCommandLine::UnregisterForProtocols(&Error);
  }
#else
  if (Register)
  {
    // AppImage: the image, not its temporary mount point
    QString Exec = qEnvironmentVariable("APPIMAGE");
    if (Exec.isEmpty())
    {
      Exec = QCoreApplication::applicationFilePath();
    }
    QString Content =
      QStringLiteral("[Desktop Entry]\nType=Application\nVersion=1.0\nName=NixSCP\n"
                     "Comment=Open file transfer URL addresses in NixSCP\n"
                     "Exec=%1 %u\nIcon=nixscp\nTerminal=false\nNoDisplay=true\nMimeType=%2;\n")
        .arg(UrlHandlerExecQuote(Exec), MimeTypes.join(QLatin1Char(';')));
    QFile File(FileName);
    if (!QDir().mkpath(Dir) || !File.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text) ||
        (File.write(Content.toUtf8()) < 0))
    {
      Error = QStringLiteral("Cannot create file \"%1\".").arg(FileName);
    }
    else
    {
      File.close();
      QProcess::execute(QStringLiteral("update-desktop-database"), {Dir});
      int Result = QProcess::execute(QStringLiteral("xdg-mime"), QStringList{QStringLiteral("default"), UrlHandlerDesktopFile} + MimeTypes);
      if (Result != 0)
      {
        Error = (Result < 0) ?
          QStringLiteral("The xdg-mime program (xdg-utils) was not found. NixSCP was registered, but it could not be made the default handler.") :
          QStringLiteral("The xdg-mime program failed (exit code %1). NixSCP was registered, but it could not be made the default handler.").arg(Result);
      }
    }
  }
  else
  {
    if (QFile::exists(FileName) && !QFile::remove(FileName))
    {
      Error = QStringLiteral("Cannot delete file \"%1\".").arg(FileName);
    }
    // xdg-mime default writes to $XDG_CONFIG_HOME/mimeapps.list; older
    // desktops used $XDG_DATA_HOME/applications/mimeapps.list
    for (const QString & List : {QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/mimeapps.list"),
                                 Dir + QStringLiteral("/mimeapps.list")})
    {
      if (!RemoveUrlHandlerAssociations(List) && Error.isEmpty())
      {
        Error = QStringLiteral("Cannot update file \"%1\".").arg(List);
      }
    }
    QProcess::execute(QStringLiteral("update-desktop-database"), {Dir});
  }
#endif
  QApplication::restoreOverrideCursor();
  if (!Error.isEmpty())
  {
    QMessageBox::warning(this, windowTitle(), Error);
  }
}

void PreferencesDialog::CreateDesktopIcon()
{
  QString Dir = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
#ifdef Q_OS_WIN
  // a shell link (QFile::link creates .lnk files on Windows)
  QString Link = Dir + QStringLiteral("/NixSCP.lnk");
  if (QMessageBox::question(this, windowTitle(),
        QStringLiteral("Create a desktop icon \"%1\"?").arg(QDir::toNativeSeparators(Link))) != QMessageBox::Yes)
  {
    return;
  }
  QFile::remove(Link);
  if (!QFile::link(QCoreApplication::applicationFilePath(), Link))
  {
    QMessageBox::warning(this, windowTitle(), QStringLiteral("Cannot create file \"%1\".").arg(QDir::toNativeSeparators(Link)));
  }
  return;
#endif
  QString FileName = Dir + QStringLiteral("/nixscp.desktop");
  if (QMessageBox::question(this, windowTitle(),
        QStringLiteral("Create a desktop icon \"%1\"?").arg(FileName)) != QMessageBox::Yes)
  {
    return;
  }
  QFile File(FileName);
  if (!File.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
  {
    QMessageBox::warning(this, windowTitle(), QStringLiteral("Cannot create file \"%1\".").arg(FileName));
    return;
  }
  File.write(QStringLiteral("[Desktop Entry]\nType=Application\nName=NixSCP\nComment=SFTP, FTP and SCP client\n"
                            "Exec=\"%1\"\nIcon=nixscp\nTerminal=false\nCategories=Network;FileTransfer;\n")
               .arg(QCoreApplication::applicationFilePath()).toUtf8());
  File.close();
  File.setPermissions(File.permissions() | QFileDevice::ExeUser);
}

void PreferencesDialog::accept()
{
  if (!SaveStorage())
  {
    return;
  }
  SaveConfiguration();
  d->CustomCommands->SaveConfiguration();
  Configuration & C = Configuration::Instance();
  if (!C.Save())
  {
    QMessageBox::critical(this, windowTitle(), C.LastSaveError());
  }
  if (d->MoveStorage)
  {
    // TConfiguration::MoveStorage, after everything else is saved
    QString Error;
    if (!C.MoveStorage(d->MoveStorageCustom, d->MoveStorageUseExisting, &Error))
    {
      QMessageBox::critical(this, windowTitle(), Error);
    }
  }
  QDialog::accept();
}

bool PreferencesDialog::SaveStorage()
{
  // Decides whether (and how) to switch the configuration file
  // (TPreferencesDialog::SaveConfiguration + CustomIniFileStorageChanged)
  Configuration & C = Configuration::Instance();
  d->MoveStorage = false;
  QString Custom;
  if (d->AutomaticIniFileStorageButton->isChecked())
  {
    Custom.clear();
  }
  else if (d->CustomIniFileStorageButton->isChecked())
  {
    Custom = d->CustomIniFileStorageEdit->text().trimmed();
    if (Custom.isEmpty())
    {
      SelectPage(QStringLiteral("Storage"));
      d->CustomIniFileStorageEdit->setFocus();
      return false;
    }
  }
  else
  {
    // explicit (/ini=) configuration file stays
    return true;
  }
  if (C.ExplicitIniFileStorageName().isEmpty() && (Custom == C.CustomIniFileStorageName()))
  {
    return true;
  }
  bool UseExisting = false;
  if (!Custom.isEmpty() && QFileInfo(Custom).isFile() &&
      (QFileInfo(Custom).absoluteFilePath() != QFileInfo(C.IniFileName()).absoluteFilePath()))
  {
    // CUSTOM_INI_FILE_OVERWRITE (the configuration is reloaded in place, no restart)
    QMessageBox Box(QMessageBox::Question, windowTitle(),
      QStringLiteral("Do you want to overwrite an existing INI file '%1'?").arg(Custom), QMessageBox::NoButton, this);
    Box.setInformativeText(QStringLiteral(
      "Choose 'Overwrite' to overwrite the selected INI file with the current configuration.\n\n"
      "Choose 'Use' to load the configuration from the selected INI file. Your current configuration "
      "will be preserved and you can revert to it, if needed."));
    QPushButton * OverwriteButton = Box.addButton(QStringLiteral("&Overwrite"), QMessageBox::YesRole);
    QPushButton * UseButton = Box.addButton(QStringLiteral("&Use"), QMessageBox::NoRole);
    Box.addButton(QMessageBox::Cancel);
    Box.exec();
    if (Box.clickedButton() == UseButton)
    {
      UseExisting = true;
    }
    else if (Box.clickedButton() != OverwriteButton)
    {
      SelectPage(QStringLiteral("Storage"));
      return false;
    }
  }
  d->MoveStorage = true;
  d->MoveStorageCustom = Custom;
  d->MoveStorageUseExisting = UseExisting;
  return true;
}

void PreferencesDialog::LoadLanguages()
{
  // TPreferencesDialog::LoadLanguages / TGUIConfiguration::GetLocales: the
  // translations built in (resources/translations), "English name - native name"
  Configuration & C = Configuration::Instance();
  unsigned int Default = Translator::DefaultLcid();
  unsigned int Current = (C.Locale == 0) ? Default : static_cast<unsigned int>(C.Locale);
  if ((Current != Translator::InternalLcid) && (Translator::FindLanguage(Current) == nullptr))
  {
    Current = Translator::InternalLcid;
  }
  else if (const Translator::LanguageInfo * Info = Translator::FindLanguage(Current))
  {
    Current = Info->Lcid;
  }
  struct Entry
  {
    QString Name;
    unsigned int Lcid;
    int Completeness;
  };
  QList<Entry> Items = { { Translator::DisplayName(Translator::InternalLcid), Translator::InternalLcid, 100 } };
  for (const Translator::LanguageInfo & Language : Translator::Languages())
  {
    if (Language.Completeness > 0)
    {
      Items.append({ Translator::DisplayName(Language.Lcid), Language.Lcid, Language.Completeness });
    }
  }
  // LocalesCompare
  std::sort(Items.begin(), Items.end(), [](const Entry & A, const Entry & B) { return A.Name.compare(B.Name, Qt::CaseInsensitive) < 0; });
  d->LanguagesView->clear();
  for (const Entry & E : Items)
  {
    auto * Item = new QListWidgetItem(E.Name, d->LanguagesView);
    Item->setData(Qt::UserRole, E.Lcid);
    // LanguagesViewCustomDrawItem: the default language bold, incomplete translations gray
    if (E.Lcid == Default)
    {
      QFont Font = Item->font();
      Font.setBold(true);
      Item->setFont(Font);
    }
    // (NixSCP: the completeness is relative to the most complete translation
    // and differs a little between versions, so only clearly incomplete ones)
    if (E.Completeness < 90)
    {
      Item->setForeground(d->LanguagesView->palette().brush(QPalette::Disabled, QPalette::Text));
      Item->setToolTip(QStringLiteral("%1%").arg(E.Completeness));
    }
    if (E.Lcid == Current)
    {
      d->LanguagesView->setCurrentItem(Item);
    }
  }
}

void PreferencesDialog::UseMasterPasswordClicked()
{
  // TPreferencesDialog::UseMasterPasswordCheckClick
  if (d->UseMasterPasswordCheck->isChecked() != Security::UseMasterPassword())
  {
    if (d->UseMasterPasswordCheck->isChecked())
    {
      ChangeMasterPassword(QStringLiteral("**Master password successfully set.**\n\nYour stored passwords are secured by AES cipher now."));
    }
    else if (DoMasterPasswordDialog(this, true, nullptr))
    {
      QStringList Errors;
      Configuration::Instance().ClearMasterPassword(&Errors);
      MasterPasswordChanged(QStringLiteral("**You have deleted your master password.**\n\nYour stored passwords are not secured now."), Errors);
    }
  }
  d->UseMasterPasswordCheck->setChecked(Security::UseMasterPassword());
  UpdateControls();
}

void PreferencesDialog::ChangeMasterPassword(const QString & Message)
{
  QString NewPassword;
  if (DoMasterPasswordDialog(this, false, &NewPassword))
  {
    QStringList Errors;
    Configuration::Instance().ChangeMasterPassword(NewPassword, &Errors);
    MasterPasswordChanged(Message, Errors);
  }
  UpdateControls();
}

void PreferencesDialog::MasterPasswordChanged(const QString & AMessage, const QStringList & Errors)
{
  // "**main instruction**\n\nmore"
  QString Message = Translator::Tr(AMessage);
  if (!Errors.isEmpty())
  {
    Message = Translator::Tr(QStringLiteral(
      "%s\n\nThere were some errors when encrypting passwords using new master password or decrypting passwords.")).replace(QStringLiteral("%s"), Message);
  }
  QString Main = Message;
  QString More;
  if (Message.startsWith(QStringLiteral("**")))
  {
    int End = Message.indexOf(QStringLiteral("**"), 2);
    Main = Message.mid(2, End - 2);
    More = Message.mid(End + 2).trimmed();
  }
  QMessageBox Box(Errors.isEmpty() ? QMessageBox::Information : QMessageBox::Warning, windowTitle(), Main, QMessageBox::Ok, this);
  Box.setInformativeText(More);
  if (!Errors.isEmpty())
  {
    Box.setDetailedText(Errors.join(QLatin1Char('\n')));
  }
  Box.exec();
}
