// Port of WinSCP source/forms/CustomCommand.cpp + CustomCommand.dfm
// (keyboard shortcut combo: windows/UserInterface.cpp InitializeShortCutCombo,
// SetShortCutCombo; pattern hint label: windows/VCLCommon.cpp HintLabel)
#include "gui/CustomCommandDialog.h"
#include "gui/HelpSystem.h"
#include "gui/EditMaskDialog.h"
#include "gui/LocationProfilesDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QToolTip>
#include <QVBoxLayout>

static const QString CommandHistoryKey = QStringLiteral("CustomCommand");
static const int scShift = 0x2000;
static const int scCtrl = 0x4000;
static const int scAlt = 0x8000;

namespace
{

// HintLabel: link-like right-aligned label showing the hint on hover/click
class PatternsHintLabel : public QLabel
{
public:
  PatternsHintLabel(const QString & Caption, const QString & Hint, QWidget * Parent) :
    QLabel(QStringLiteral("<a href=\"#\">%1</a>").arg(Caption.toHtmlEscaped()), Parent)
  {
    setToolTip(QStringLiteral("<pre>%1</pre>").arg(Hint.toHtmlEscaped()));
    setTextFormat(Qt::RichText);
    setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    connect(this, &QLabel::linkActivated, this, [this]()
    {
      QToolTip::showText(mapToGlobal(QPoint(0, height())), toolTip(), this);
    });
  }
};

}

QString CustomCommandDialog::PatternsHint()
{
  QStringList Lines = {
    QStringLiteral("Patterns:"),
    QStringLiteral("!! expands to exclamation mark"),
    QStringLiteral("! expands to file name"),
    QStringLiteral("!& expands to list of selected files (quoted, space-delimited)"),
    QStringLiteral("!/ expands to current remote path"),
    QStringLiteral("!S expands to current session URL"),
    QStringLiteral("!@ expands to current session hostname"),
    QStringLiteral("!U expands to current session username"),
    QStringLiteral("!P expands to current session password"),
    QStringLiteral("!# expands to current session port number"),
    QStringLiteral("!N expands to current session name"),
    QStringLiteral("!?prompt[\\]?default! expands to user-entered value with given prompt and default (optional \\ avoids escaping)"),
    QStringLiteral("!`command` expands to output of local command"),
    QStringLiteral(" "),
    QStringLiteral("Local command patterns:"),
    QStringLiteral("!^! expands to file name from local panel"),
    QStringLiteral(" "),
    QStringLiteral("Example:"),
    QStringLiteral("grep \"!?Pattern:?!\" !&"),
  };
  Lines.insert(6, QStringLiteral("!E expands to serialized connection data of current session"));
  Lines.insert(11, QStringLiteral("!K expands to current session private key path"));
  Lines.insert(18, QStringLiteral("!\\ expands to current local path"));
  return Lines.join(QLatin1Char('\n'));
}

CustomCommandDialog::CustomCommandDialog(QWidget * Parent, const CustomCommandList & List, Mode AMode, int Options,
    const QList<int> & UsedShortCuts, ValidateCallback OnValidate) :
  QDialog(Parent),
  FCustomCommandList(List),
  FMode(AMode),
  FOptions(Options),
  FOnValidate(std::move(OnValidate))
{
  switch (FMode)
  {
    case ccmAdd:
      setWindowTitle(QStringLiteral("Add Custom Command"));
      break;
    case ccmEdit:
      setWindowTitle(QStringLiteral("Edit Custom Command"));
      break;
    case ccmAdHoc:
    default:
      setWindowTitle(QStringLiteral("Custom Command"));
      break;
  }

  QVBoxLayout * Layout = new QVBoxLayout(this);
  QGroupBox * Group = new QGroupBox(this);
  QGridLayout * Grid = new QGridLayout(Group);
  Grid->setColumnStretch(0, 1);
  Grid->setColumnStretch(1, 1);
  Grid->setColumnMinimumWidth(0, 200);
  Grid->setColumnMinimumWidth(1, 215);

  DescriptionEdit = new QLineEdit(Group);
  DescriptionLabel = new QLabel(QStringLiteral("&Description:"), Group);
  DescriptionLabel->setBuddy(DescriptionEdit);
  Grid->addWidget(DescriptionLabel, 0, 0, 1, 2);
  Grid->addWidget(DescriptionEdit, 1, 0, 1, 2);

  CommandEdit = new QComboBox(Group);
  CommandEdit->setEditable(true);
  CommandEdit->setInsertPolicy(QComboBox::NoInsert);
  CommandEdit->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  CommandEdit->setMinimumContentsLength(40);
  CommandEdit->lineEdit()->setMaxLength(2048);
  EditMaskDialog::LoadHistoryCombo(CommandEdit, CommandHistoryKey);
  QLabel * CommandLabel = new QLabel(QStringLiteral("&Custom command:"), Group);
  CommandLabel->setBuddy(CommandEdit);
  Grid->addWidget(CommandLabel, 2, 0, 1, 2);
  Grid->addWidget(CommandEdit, 3, 0, 1, 2);
  PatternsHintLabel * HintText = new PatternsHintLabel(QStringLiteral("patterns"), PatternsHint(), Group);
  Grid->addWidget(HintText, 4, 1, Qt::AlignRight);

  RemoteCommandButton = new QRadioButton(QStringLiteral("&Remote command"), Group);
  LocalCommandButton = new QRadioButton(QStringLiteral("&Local command"), Group);
  Grid->addWidget(RemoteCommandButton, 5, 0);
  Grid->addWidget(LocalCommandButton, 5, 1);

  ApplyToDirectoriesCheck = new QCheckBox(QStringLiteral("&Apply to directories"), Group);
  RecursiveCheck = new QCheckBox(QStringLiteral("&Execute recursively"), Group);
  Grid->addWidget(ApplyToDirectoriesCheck, 6, 0);
  Grid->addWidget(RecursiveCheck, 6, 1);
  ShowResultsCheck = new QCheckBox(QStringLiteral("&Show results in terminal"), Group);
  RemoteFilesCheck = new QCheckBox(QStringLiteral("&Use remote files"), Group);
  Grid->addWidget(ShowResultsCheck, 7, 0);
  Grid->addWidget(RemoteFilesCheck, 7, 1);
  CopyResultsCheck = new QCheckBox(QStringLiteral("Copy results to clip&board"), Group);
  Grid->addWidget(CopyResultsCheck, 8, 0);

  ShortCutCombo = new QComboBox(Group);
  ShortCutCombo->setMaxVisibleItems(qMax(ShortCutCombo->maxVisibleItems(), 16));
  ShortCutLabel = new QLabel(QStringLiteral("&Keyboard shortcut:"), Group);
  ShortCutLabel->setBuddy(ShortCutCombo);
  Grid->addWidget(ShortCutLabel, 9, 0);
  Grid->addWidget(ShortCutCombo, 9, 1);

  Layout->addWidget(Group);

  QDialogButtonBox * Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Help, this);
  OkButton = Buttons->button(QDialogButtonBox::Ok);
  Buttons->button(QDialogButtonBox::Help)->setText(QStringLiteral("&Help"));
  Layout->addWidget(Buttons);
  connect(Buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  HelpSystem::InstallFormHelp(this, QStringLiteral("ui_customcommand"), Buttons->button(QDialogButtonBox::Help));

  // tab order as in the .dfm
  setTabOrder(DescriptionEdit, CommandEdit);
  setTabOrder(CommandEdit, HintText);
  setTabOrder(HintText, RemoteCommandButton);
  setTabOrder(RemoteCommandButton, LocalCommandButton);
  setTabOrder(LocalCommandButton, ApplyToDirectoriesCheck);
  setTabOrder(ApplyToDirectoriesCheck, RecursiveCheck);
  setTabOrder(RecursiveCheck, ShowResultsCheck);
  setTabOrder(ShowResultsCheck, RemoteFilesCheck);
  setTabOrder(RemoteFilesCheck, CopyResultsCheck);
  setTabOrder(CopyResultsCheck, ShortCutCombo);

  if (FMode == ccmAdHoc)
  {
    DescriptionLabel->setVisible(false);
    DescriptionEdit->setVisible(false);
    ShortCutLabel->setVisible(false);
    ShortCutCombo->setVisible(false);
  }
  else
  {
    // InitializeShortCutCombo
    ShortCutCombo->addItem(QStringLiteral("None"), 0);
    QList<int> Available;
    for (int Key = '0'; Key <= '9'; Key++)
    {
      Available.append(scCtrl | Key);
    }
    for (int Key = 'A'; Key <= 'Z'; Key++)
    {
      Available.append(scShift | scCtrl | scAlt | Key);
    }
    for (int ShortCut : Available)
    {
      if (!UsedShortCuts.contains(ShortCut))
      {
        ShortCutCombo->addItem(LocationProfilesDialog::ShortCutToText(ShortCut), ShortCut);
      }
    }
  }

  auto Changed = [this]() { UpdateControls(); };
  connect(DescriptionEdit, &QLineEdit::textChanged, this, Changed);
  connect(CommandEdit, &QComboBox::editTextChanged, this, Changed);
  connect(RemoteCommandButton, &QRadioButton::toggled, this, Changed);
  connect(LocalCommandButton, &QRadioButton::toggled, this, Changed);
  connect(ApplyToDirectoriesCheck, &QCheckBox::toggled, this, Changed);
  connect(RecursiveCheck, &QCheckBox::toggled, this, Changed);
  connect(ShowResultsCheck, &QCheckBox::toggled, this, Changed);
  connect(RemoteFilesCheck, &QCheckBox::toggled, this, Changed);
  connect(CopyResultsCheck, &QCheckBox::toggled, this, Changed);
}

CustomCommandDialog::~CustomCommandDialog() = default;

void CustomCommandDialog::UpdateControls()
{
  RemoteCommandButton->setEnabled((FOptions & ccoDisableRemote) == 0);

  QString Command = CommandEdit->currentText();
  OkButton->setEnabled(!Command.isEmpty() && !DescriptionEdit->text().isEmpty());

  bool RemoteCommand = RemoteCommandButton->isChecked();
  bool AllowRecursive = true;
  bool AllowApplyToDirectories = true;
  bool AllowRemoteFiles = false;
  CustomCommandInfo Info = AnalyzeCustomCommand(Command, !RemoteCommand);
  if (Info.Valid)
  {
    AllowRemoteFiles = !RemoteCommand && Info.RemoteFileCommand;
    AllowRecursive = Info.FileCommand && !Info.FileListCommand;
    if (AllowRecursive && !RemoteCommand)
    {
      AllowRecursive = !Info.LocalFileName;
    }
    AllowApplyToDirectories = Info.FileCommand;
  }

  RecursiveCheck->setEnabled(AllowRecursive && (!RemoteFilesCheck->isEnabled() || !RemoteFilesCheck->isChecked()));
  ApplyToDirectoriesCheck->setEnabled(AllowApplyToDirectories);
  ShowResultsCheck->setEnabled(RemoteCommand);
  RemoteFilesCheck->setEnabled(
    ((FOptions & ccoDisableRemoteFiles) == 0) && AllowRemoteFiles &&
    (!RecursiveCheck->isEnabled() || !RecursiveCheck->isChecked()));
}

void CustomCommandDialog::SetParams(int Value)
{
  FParams = Value;
  ApplyToDirectoriesCheck->setChecked((Value & ccApplyToDirectories) != 0);
  RecursiveCheck->setChecked((Value & ccRecursive) != 0);
  (((Value & ccLocal) != 0) ? LocalCommandButton : RemoteCommandButton)->setChecked(true);
  ShowResultsCheck->setChecked((Value & ccShowResults) != 0);
  CopyResultsCheck->setChecked((Value & ccCopyResults) != 0);
  RemoteFilesCheck->setChecked((Value & ccRemoteFiles) != 0);
}

int CustomCommandDialog::GetParams() const
{
  return
    (FParams & ~(ccApplyToDirectories | ccRecursive | ccLocal | ccShowResults | ccCopyResults | ccRemoteFiles)) |
    (!RemoteCommandButton->isChecked() ? ccLocal : 0) |
    (ApplyToDirectoriesCheck->isChecked() ? ccApplyToDirectories : 0) |
    ((RecursiveCheck->isChecked() && RecursiveCheck->isEnabled()) ? ccRecursive : 0) |
    ((ShowResultsCheck->isChecked() && ShowResultsCheck->isEnabled()) ? ccShowResults : 0) |
    ((CopyResultsCheck->isChecked() && CopyResultsCheck->isEnabled()) ? ccCopyResults : 0) |
    ((RemoteFilesCheck->isChecked() && RemoteFilesCheck->isEnabled()) ? ccRemoteFiles : 0);
}

void CustomCommandDialog::SetShortCut(int ShortCut)
{
  // SetShortCutCombo: select the value, inserting it in order when not offered
  for (int Index = ShortCutCombo->count() - 1; Index >= 0; Index--)
  {
    int AShortCut = ShortCutCombo->itemData(Index).toInt();
    if (AShortCut == ShortCut)
    {
      ShortCutCombo->setCurrentIndex(Index);
      break;
    }
    else if (AShortCut < ShortCut)
    {
      ShortCutCombo->insertItem(Index + 1, LocationProfilesDialog::ShortCutToText(ShortCut), ShortCut);
      ShortCutCombo->setCurrentIndex(Index + 1);
      break;
    }
  }
}

void CustomCommandDialog::GetCommand(CustomCommandType & Command) const
{
  Command.Name = DescriptionEdit->text();
  Command.Command = CommandEdit->currentText();
  Command.Params = GetParams();
  if (FMode != ccmAdHoc)
  {
    Command.ShortCut = ShortCutCombo->currentData().toInt();
  }
}

void CustomCommandDialog::accept()
{
  // FormCloseQuery
  if ((FMode == ccmAdd) || (FMode == ccmEdit))
  {
    QString Desc = DescriptionEdit->text();
    if (Desc.contains(QLatin1Char('=')))
    {
      DescriptionEdit->setFocus();
      QMessageBox::critical(this, QStringLiteral("Error"), QStringLiteral("Custom command description cannot contain '%1'.").arg(QStringLiteral("=")));
      return;
    }

    if (((FMode == ccmAdd) || ((FMode == ccmEdit) && (Desc != FOrigDescription))) &&
        (FCustomCommandList.Find(Desc) != nullptr))
    {
      DescriptionEdit->setFocus();
      QMessageBox::critical(this, QStringLiteral("Error"), QStringLiteral("Custom command with description '%1' already exists.").arg(Desc));
      return;
    }
  }

  QString Error = ValidateCustomCommand(CommandEdit->currentText(), !RemoteCommandButton->isChecked());
  if (!Error.isEmpty())
  {
    CommandEdit->setFocus();
    QMessageBox::critical(this, QStringLiteral("Error"), Error);
    return;
  }

  if (FOnValidate)
  {
    CustomCommandType Command;
    GetCommand(Command);
    Error = FOnValidate(Command);
    if (!Error.isEmpty())
    {
      QMessageBox::critical(this, QStringLiteral("Error"), Error);
      return;
    }
  }

  QDialog::accept();
}

bool CustomCommandDialog::Execute(QWidget * Parent, CustomCommandType & Command, const CustomCommandList & List, Mode AMode,
  int Options, const QList<int> & UsedShortCuts, ValidateCallback OnValidate)
{
  CustomCommandDialog Dialog(Parent, List, AMode, Options, UsedShortCuts, std::move(OnValidate));
  QString Name = Command.Name;
  if ((AMode == ccmAdHoc) && Name.isEmpty())
  {
    Name = QStringLiteral("Ad Hoc");
  }
  Dialog.DescriptionEdit->setText(Name);
  Dialog.FOrigDescription = Name;
  Dialog.CommandEdit->setEditText(Command.Command);
  int Params = Command.Params;
  if ((Options & ccoDisableRemote) != 0)
  {
    Params |= ccLocal;
  }
  Dialog.SetParams(Params);
  if (AMode != ccmAdHoc)
  {
    Dialog.SetShortCut(Command.ShortCut);
  }
  Dialog.UpdateControls();
  if (AMode == ccmAdHoc)
  {
    Dialog.CommandEdit->setFocus();
  }
  else
  {
    Dialog.DescriptionEdit->setFocus();
  }
  Dialog.adjustSize();

  bool Result = (Dialog.exec() == QDialog::Accepted);
  if (Result)
  {
    Dialog.GetCommand(Command);
    EditMaskDialog::SaveToHistory(CommandHistoryKey, Command.Command);
  }
  return Result;
}
