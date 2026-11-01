// Port of WinSCP source/forms/Custom.cpp (TSaveWorkspaceDialog) and
// source/forms/CustomScpExplorer.cpp (SaveWorkspace, DoSaveWorkspace)
#include "gui/SaveWorkspaceDialog.h"
#include "gui/HelpSystem.h"

#include "core/Configuration.h"
#include "core/Workspaces.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>

SaveWorkspaceDialog::SaveWorkspaceDialog(QWidget * Parent, const StoredSessionList & Sessions, bool CanSavePasswords,
    bool NotRecommendedSavingPasswords) :
  QDialog(Parent),
  FSessions(Sessions)
{
  // SAVE_WORKSPACE_CAPTION
  setWindowTitle(QStringLiteral("Save workspace as"));
  setMinimumWidth(380);

  QVBoxLayout * Layout = new QVBoxLayout(this);

  WorkspaceNameCombo = new QComboBox(this);
  WorkspaceNameCombo->setEditable(true);
  WorkspaceNameCombo->setInsertPolicy(QComboBox::NoInsert);
  WorkspaceNameCombo->setCompleter(nullptr);   // AutoComplete = false
  WorkspaceNameCombo->setMaxVisibleItems(qMax(WorkspaceNameCombo->maxVisibleItems(), 16));
  WorkspaceNameCombo->addItems(Workspaces::WorkspaceNames(Sessions));
  QLabel * Label = new QLabel(QStringLiteral("&Save workspace as:"), this);
  Label->setBuddy(WorkspaceNameCombo);
  Layout->addWidget(Label);
  Layout->addWidget(WorkspaceNameCombo);
  Layout->addSpacing(4);

  // SAVE_WORKSPACE_PASSWORDS / SAVE_WORKSPACE_PASSWORDS_RECOMMENDED
  // (NixSCP has no master password)
  SavePasswordsCheck = new QCheckBox(
    NotRecommendedSavingPasswords ? QStringLiteral("Save &passwords (not recommended)") : QStringLiteral("Save &passwords"), this);
  SavePasswordsCheck->setEnabled(CanSavePasswords);
  Layout->addWidget(SavePasswordsCheck);

  CreateShortcutCheck = new QCheckBox(QStringLiteral("&Create desktop shortcut"), this);
  Layout->addWidget(CreateShortcutCheck);

  EnableAutoSaveCheck = new QCheckBox(QStringLiteral("Enable &automatic saving of this workspace"), this);
  Layout->addWidget(EnableAutoSaveCheck);

  Layout->addStretch();

  QDialogButtonBox * Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Help, this);
  OkButton = Buttons->button(QDialogButtonBox::Ok);
  Buttons->button(QDialogButtonBox::Help)->setText(QStringLiteral("&Help"));
  Layout->addWidget(Buttons);
  connect(Buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  // HELP_WORKSPACE_SAVE
  HelpSystem::InstallFormHelp(this, QStringLiteral("ui_save_workspace"), Buttons->button(QDialogButtonBox::Help));
  connect(WorkspaceNameCombo, &QComboBox::editTextChanged, this, [this]() { UpdateControls(); });
}

SaveWorkspaceDialog::~SaveWorkspaceDialog() = default;

void SaveWorkspaceDialog::UpdateControls()
{
  // DoChange
  OkButton->setEnabled(!WorkspaceNameCombo->currentText().isEmpty());
}

void SaveWorkspaceDialog::accept()
{
  // DoValidate
  QString Error = Workspaces::ValidateWorkspaceName(FSessions, WorkspaceNameCombo->currentText());
  if (!Error.isEmpty())
  {
    QMessageBox::critical(this, QStringLiteral("Error"), Error);
    WorkspaceNameCombo->setFocus();
    return;
  }
  QDialog::accept();
}

bool SaveWorkspaceDialog::Execute(QWidget * Parent, const StoredSessionList & Sessions, QString & WorkspaceName,
  bool * SavePasswords, bool NotRecommendedSavingPasswords, bool & CreateShortcut, bool & EnableAutoSave)
{
  SaveWorkspaceDialog Dialog(Parent, Sessions, (SavePasswords != nullptr), NotRecommendedSavingPasswords);
  Dialog.WorkspaceNameCombo->setEditText(WorkspaceName);
  Dialog.WorkspaceNameCombo->lineEdit()->selectAll();
  Dialog.SavePasswordsCheck->setChecked((SavePasswords != nullptr) && *SavePasswords);
  Dialog.CreateShortcutCheck->setChecked(CreateShortcut);
  Dialog.EnableAutoSaveCheck->setChecked(EnableAutoSave);
  Dialog.UpdateControls();
  Dialog.WorkspaceNameCombo->setFocus();
  bool Result = (Dialog.exec() == QDialog::Accepted);
  if (Result)
  {
    WorkspaceName = Dialog.WorkspaceNameCombo->currentText();
    if (SavePasswords != nullptr)
    {
      *SavePasswords = Dialog.SavePasswordsCheck->isChecked();
    }
    CreateShortcut = Dialog.CreateShortcutCheck->isChecked();
    EnableAutoSave = Dialog.EnableAutoSaveCheck->isChecked();
  }
  return Result;
}

bool SaveWorkspaceDialog::SaveWorkspace(QWidget * Parent, const QList<SessionData> & Tabs, bool EnableAutoSave, QString * SavedName)
{
  Configuration & Config = Configuration::Instance();
  StoredSessionList & Sessions = Config.StoredSessions();

  Workspaces::PasswordOptions Options = Workspaces::SavePasswordOptions(Sessions, Tabs);
  bool SavePasswords = Options.SavePasswords;
  QString Name = Workspaces::WorkspaceName(Config);
  bool CreateShortcut = false;

  bool Result = Execute(Parent, Sessions, Name, (Options.CanSavePasswords ? &SavePasswords : nullptr),
    Options.NotRecommended, CreateShortcut, EnableAutoSave);
  if (Result)
  {
    if (!Options.CanSavePasswords)
    {
      SavePasswords = false;
    }
    // DoSaveWorkspace
    Workspaces::SetLastWorkspace(Name);
    Workspaces::SaveWorkspace(Sessions, Name, Tabs, SavePasswords);

    if (CreateShortcut)
    {
      CreateDesktopShortcut(Parent, Name);
    }

    if (EnableAutoSave)
    {
      Config.AutoSaveWorkspace = true;
      Config.AutoWorkspace = Name;
      if (Options.CanSavePasswords)
      {
        Config.AutoSaveWorkspacePasswords = SavePasswords;
      }
    }
    Config.Save();

    if (SavedName != nullptr)
    {
      *SavedName = Name;
    }
  }
  return Result;
}

// Desktop Entry Specification: quoting of Exec arguments
static QString DesktopExecArgument(const QString & Argument)
{
  QString Result;
  for (QChar C : Argument)
  {
    if ((C == QLatin1Char('"')) || (C == QLatin1Char('`')) || (C == QLatin1Char('$')) || (C == QLatin1Char('\\')))
    {
      Result += QLatin1Char('\\');
    }
    Result += C;
  }
  Result.replace(QLatin1Char('%'), QStringLiteral("%%"));
  return QLatin1Char('"') + Result + QLatin1Char('"');
}

bool SaveWorkspaceDialog::CreateDesktopShortcut(QWidget * Parent, const QString & WorkspaceName)
{
  QString Dir = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
  QString BaseName = WorkspaceName;
  BaseName.replace(QLatin1Char('/'), QLatin1Char('_'));
  QString FileName = Dir + QLatin1Char('/') + BaseName + QStringLiteral(".desktop");
  QFile File(FileName);
  bool Result =
    QDir().mkpath(Dir) &&
    File.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
  if (Result)
  {
    QString Name = WorkspaceName;
    Name.replace(QLatin1Char('\n'), QLatin1Char(' '));
    File.write(QStringLiteral(
      "[Desktop Entry]\nType=Application\nName=%1\nComment=NixSCP workspace\n"
      "Exec=%2 %3\nIcon=nixscp\nTerminal=false\nCategories=Network;FileTransfer;\n")
      .arg(Name, DesktopExecArgument(QCoreApplication::applicationFilePath()), DesktopExecArgument(WorkspaceName)).toUtf8());
    File.close();
    // trusted launcher
    File.setPermissions(File.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeUser);
  }
  else
  {
    QMessageBox::critical(Parent, QStringLiteral("Error"), QStringLiteral("Cannot create file \"%1\".").arg(FileName));
  }
  return Result;
}
