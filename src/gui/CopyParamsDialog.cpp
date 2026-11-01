// Port of WinSCP source/forms/CopyParams.{cpp,dfm} (TCopyParamsFrame),
// source/forms/CopyParamCustom.{cpp,dfm}, source/forms/CopyParamPreset.{cpp,dfm}
// and CopyParamListPopup/CopyParamListPopupClick of source/windows/WinInterface.cpp.
// The permissions popup of source/forms/Rights.cpp is a menu of checkable actions.
// Windows policy: "Preserve read-only" (Download options) is adapted (the
// local file loses its write bits); removed: Clear 'Archive' attribute,
// Replace '\:*?'... (no Linux equivalent; their ini values are preserved).
// Not ported: Encrypt new files (NixSCP sites have no file encryption key),
// Generate Code... (presets menu).
#include "gui/CopyParamsDialog.h"
#include "gui/HelpSystem.h"
#include "gui/EditMaskDialog.h"
#include "gui/PreferencesDialog.h"

#include "core/Configuration.h"
#include "core/FileSystem.h"
#include "core/RemoteFile.h"

#include <QAction>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFont>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QShowEvent>
#include <QToolButton>
#include <QTreeWidget>
#include <QKeyEvent>
#include <QVBoxLayout>

//---------------------------------------------------------------------------
// CopyParamsFrame
//---------------------------------------------------------------------------

struct CopyParamsFrame::Private
{
  int CopyParamAttrs = 0;
  CopyParam Params;
  unsigned int Rights = 0644;
  bool RemoveCtrlZ = false;   // kept when only BOM removal is applicable

  QGroupBox * TransferModeGroup = nullptr;
  QRadioButton * TMTextButton = nullptr;
  QRadioButton * TMBinaryButton = nullptr;
  QRadioButton * TMAutomaticButton = nullptr;
  QLabel * AsciiFileMaskLabel = nullptr;
  QComboBox * AsciiFileMaskCombo = nullptr;

  QGroupBox * ChangeCaseGroup = nullptr;
  QRadioButton * CCNoChangeButton = nullptr;
  QRadioButton * CCUpperCaseButton = nullptr;
  QRadioButton * CCLowerCaseButton = nullptr;
  QRadioButton * CCLowerCaseShortButton = nullptr;

  QGroupBox * RemotePropertiesGroup = nullptr;
  QCheckBox * PreserveRightsCheck = nullptr;
  QLineEdit * RightsEdit = nullptr;
  QToolButton * RightsButton = nullptr;
  QAction * RightActions[12] = {};   // rwx x 3, then SUID, SGID, sticky
  QAction * AddXToDirectoriesAction = nullptr;
  QCheckBox * IgnorePermErrorsCheck = nullptr;
  QCheckBox * RemoveCtrlZAndBOMCheck = nullptr;

  QGroupBox * LocalPropertiesGroup = nullptr;
  QCheckBox * PreserveReadOnlyCheck = nullptr;

  QGroupBox * CommonPropertiesGroup = nullptr;
  QCheckBox * PreserveTimeCheck = nullptr;
  QCheckBox * PreserveTimeDirsCheck = nullptr;
  QCheckBox * CommonCalculateSizeCheck = nullptr;
  QLabel * SpeedLabel = nullptr;
  QComboBox * SpeedCombo = nullptr;

  QGroupBox * OtherGroup = nullptr;
  QLabel * IncludeFileMaskLabel = nullptr;
  QComboBox * IncludeFileMaskCombo = nullptr;
  QPushButton * IncludeFileMaskButton = nullptr;
  QCheckBox * NewerOnlyCheck = nullptr;
  QCheckBox * ExcludeHiddenFilesCheck = nullptr;
  QCheckBox * ExcludeEmptyDirectoriesCheck = nullptr;

  bool Attr(int Flag) const { return (CopyParamAttrs & Flag) != 0; }
  void UpdateControls();
  void UpdateRightsControls();
  bool UpdateRightsByStr();
};

static unsigned int RightBit(int Index)
{
  static const unsigned int Bits[12] = { 0400, 0200, 0100, 0040, 0020, 0010, 0004, 0002, 0001, 04000, 02000, 01000 };
  return Bits[Index];
}

void CopyParamsFrame::Private::UpdateRightsControls()
{
  RightsEdit->setText(RightsToText(Rights));
  for (int I = 0; I < 12; I++)
  {
    const QSignalBlocker Blocker(RightActions[I]);
    RightActions[I]->setChecked((Rights & RightBit(I)) != 0);
  }
}

bool CopyParamsFrame::Private::UpdateRightsByStr()
{
  // TRightsFrame accepts both "rw-r--r--" and octal
  QString Text = RightsEdit->text().trimmed();
  unsigned int Mode = 0;
  if (RightsFromText(Text, Mode) || Rights::FromOctal(Text, Mode))
  {
    Rights = Mode;
    UpdateRightsControls();
    return true;
  }
  return false;
}

void CopyParamsFrame::Private::UpdateControls()
{
  // TCopyParamsFrame::UpdateControls
  bool NotMaskOnly = !Attr(cpaIncludeMaskOnly);
  CommonPropertiesGroup->setEnabled(NotMaskOnly);
  LocalPropertiesGroup->setEnabled(NotMaskOnly);
  RemotePropertiesGroup->setEnabled(NotMaskOnly);
  TransferModeGroup->setEnabled(!Attr(cpaNoTransferMode) && NotMaskOnly);
  AsciiFileMaskLabel->setEnabled(TransferModeGroup->isEnabled() && TMAutomaticButton->isChecked());
  AsciiFileMaskCombo->setEnabled(TransferModeGroup->isEnabled() && TMAutomaticButton->isChecked());
  PreserveRightsCheck->setEnabled(NotMaskOnly && !Attr(cpaNoRights));
  RightsEdit->setEnabled(PreserveRightsCheck->isChecked() && PreserveRightsCheck->isEnabled());
  RightsButton->setEnabled(RightsEdit->isEnabled());
  PreserveReadOnlyCheck->setEnabled(NotMaskOnly && !Attr(cpaNoPreserveReadOnly));
  IncludeFileMaskCombo->setEnabled(!Attr(cpaNoIncludeMask) || Attr(cpaIncludeMaskOnly));
  IncludeFileMaskLabel->setEnabled(IncludeFileMaskCombo->isEnabled());
  IncludeFileMaskButton->setEnabled(IncludeFileMaskCombo->isEnabled());
  // Do not mention EOF when not supported, but only when at least BOM is supported
  RemoveCtrlZAndBOMCheck->setText((Attr(cpaNoRemoveCtrlZ) && !Attr(cpaNoRemoveBOM)) ?
    QStringLiteral("Remo&ve BOM mark") : QStringLiteral("Remo&ve BOM and EOF marks"));
  RemoveCtrlZAndBOMCheck->setEnabled(
    (!Attr(cpaNoRemoveCtrlZ) || !Attr(cpaNoRemoveBOM)) && NotMaskOnly &&
    (TMTextButton->isChecked() || TMAutomaticButton->isChecked()));
  PreserveTimeCheck->setEnabled(!Attr(cpaNoPreserveTime) && NotMaskOnly);
  // can be enabled even when "preserve time" is not (Synchronize dialog)
  PreserveTimeDirsCheck->setEnabled(!Attr(cpaNoPreserveTimeDirs) && NotMaskOnly && PreserveTimeCheck->isChecked());
  CommonCalculateSizeCheck->setEnabled(!Attr(cpaNoCalculateSize) && NotMaskOnly);
  ChangeCaseGroup->setEnabled(NotMaskOnly);
  IgnorePermErrorsCheck->setEnabled(
    ((PreserveRightsCheck->isEnabled() && PreserveRightsCheck->isChecked()) ||
     (PreserveTimeCheck->isEnabled() && PreserveTimeCheck->isChecked())) &&
    !Attr(cpaNoIgnorePermErrors) && NotMaskOnly);
  NewerOnlyCheck->setEnabled(NotMaskOnly && !Attr(cpaNoNewerOnly));
  ExcludeHiddenFilesCheck->setEnabled(IncludeFileMaskCombo->isEnabled());
  ExcludeEmptyDirectoriesCheck->setEnabled(NotMaskOnly);
}

static QComboBox * NewHistoryCombo(int MaxLength)
{
  auto * Combo = new QComboBox();
  Combo->setEditable(true);
  Combo->setInsertPolicy(QComboBox::NoInsert);
  Combo->lineEdit()->setMaxLength(MaxLength);
  return Combo;
}

static QStringList DefaultSpeedLimits()
{
  return { QStringLiteral("Unlimited"), QStringLiteral("1024"), QStringLiteral("512"), QStringLiteral("256"),
    QStringLiteral("128"), QStringLiteral("64"), QStringLiteral("32"), QStringLiteral("16"), QStringLiteral("8") };
}

CopyParamsFrame::CopyParamsFrame(QWidget * Parent) :
  QWidget(Parent),
  d(new Private)
{
  // Transfer mode
  d->TransferModeGroup = new QGroupBox(QStringLiteral("Transfer mode"));
  d->TMTextButton = new QRadioButton(QStringLiteral("&Text (plain text, html, scripts, ...)"));
  d->TMBinaryButton = new QRadioButton(QStringLiteral("&Binary (archives, doc, ...)"));
  d->TMAutomaticButton = new QRadioButton(QStringLiteral("&Automatic"));
  d->AsciiFileMaskLabel = new QLabel(QStringLiteral("Transfer following &files in text mode:"));
  d->AsciiFileMaskCombo = NewHistoryCombo(1000);
  d->AsciiFileMaskCombo->setMinimumContentsLength(20);
  d->AsciiFileMaskCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  d->AsciiFileMaskLabel->setBuddy(d->AsciiFileMaskCombo);
  auto * ModeButtons = new QButtonGroup(this);
  ModeButtons->addButton(d->TMTextButton);
  ModeButtons->addButton(d->TMBinaryButton);
  ModeButtons->addButton(d->TMAutomaticButton);
  auto * ModeLayout = new QVBoxLayout(d->TransferModeGroup);
  ModeLayout->addWidget(d->TMTextButton);
  ModeLayout->addWidget(d->TMBinaryButton);
  ModeLayout->addWidget(d->TMAutomaticButton);
  ModeLayout->addWidget(d->AsciiFileMaskLabel);
  ModeLayout->addWidget(d->AsciiFileMaskCombo);
  ModeLayout->addStretch();

  // Filename modification
  d->ChangeCaseGroup = new QGroupBox(QStringLiteral("Filename modification"));
  d->CCNoChangeButton = new QRadioButton(QStringLiteral("No chan&ge"));
  d->CCUpperCaseButton = new QRadioButton(QStringLiteral("&Upper case"));
  d->CCLowerCaseButton = new QRadioButton(QStringLiteral("Lo&wer case"));
  d->CCLowerCaseShortButton = new QRadioButton(QStringLiteral("Lower case &8.3"));
  auto * CaseButtons = new QButtonGroup(this);
  auto * CaseLayout = new QVBoxLayout(d->ChangeCaseGroup);
  for (QRadioButton * B : {d->CCNoChangeButton, d->CCUpperCaseButton, d->CCLowerCaseButton, d->CCLowerCaseShortButton})
  {
    CaseButtons->addButton(B);
    CaseLayout->addWidget(B);
  }
  CaseLayout->addStretch();

  // Upload options
  d->RemotePropertiesGroup = new QGroupBox(QStringLiteral("Upload options"));
  d->PreserveRightsCheck = new QCheckBox(QStringLiteral("Set pe&rmissions:"));
  d->RightsEdit = new QLineEdit();
  d->RightsEdit->setMaxLength(9);
  d->RightsButton = new QToolButton();
  d->RightsButton->setText(QStringLiteral("..."));
  d->RightsButton->setToolTip(QStringLiteral("Configure permissions"));
  d->RightsButton->setPopupMode(QToolButton::InstantPopup);
  auto * RightsMenu = new QMenu(d->RightsButton);
  // forms/Rights.dfm: Owner / Group / Others x Read / Write / Execute, special bits
  static const char * const RightCaptions[12] = {
    "Owner R", "Owner W", "Owner X", "Group R", "Group W", "Group X", "Others R", "Others W", "Others X",
    "Set UID", "Set GID", "Sticky bit" };
  for (int Index = 0; Index < 12; Index++)
  {
    if ((Index > 0) && ((Index % 3) == 0))
    {
      RightsMenu->addSeparator();
    }
    QAction * Action = RightsMenu->addAction(QLatin1String(RightCaptions[Index]));
    Action->setCheckable(true);
    d->RightActions[Index] = Action;
    connect(Action, &QAction::toggled, this, [this, Index](bool Checked)
    {
      unsigned int Bit = RightBit(Index);
      unsigned int NewRights = Checked ? (d->Rights | Bit) : (d->Rights & ~Bit);
      if (NewRights != d->Rights)
      {
        d->Rights = NewRights;
        d->UpdateRightsControls();
      }
    });
  }
  RightsMenu->addSeparator();
  d->AddXToDirectoriesAction = RightsMenu->addAction(QStringLiteral("Add &X to directories"));
  d->AddXToDirectoriesAction->setCheckable(true);
  d->RightsButton->setMenu(RightsMenu);
  d->IgnorePermErrorsCheck = new QCheckBox(QStringLiteral("Ign&ore permission errors"));
  d->RemoveCtrlZAndBOMCheck = new QCheckBox(QStringLiteral("Remo&ve BOM and EOF marks"));
  auto * RemoteLayout = new QVBoxLayout(d->RemotePropertiesGroup);
  RemoteLayout->addWidget(d->PreserveRightsCheck);
  auto * RightsRow = new QHBoxLayout();
  RightsRow->setContentsMargins(18, 0, 0, 0);
  RightsRow->addWidget(d->RightsEdit, 1);
  RightsRow->addWidget(d->RightsButton);
  RemoteLayout->addLayout(RightsRow);
  RemoteLayout->addWidget(d->IgnorePermErrorsCheck);
  RemoteLayout->addWidget(d->RemoveCtrlZAndBOMCheck);
  RemoteLayout->addStretch();

  // Download options
  d->LocalPropertiesGroup = new QGroupBox(QStringLiteral("Download options"));
  d->PreserveReadOnlyCheck = new QCheckBox(QStringLiteral("Preserve rea&d-only"));
  d->PreserveReadOnlyCheck->setToolTip(QStringLiteral(
    "Downloaded files are made read-only when the remote file is not writable by its owner."));
  auto * LocalLayout = new QVBoxLayout(d->LocalPropertiesGroup);
  LocalLayout->addWidget(d->PreserveReadOnlyCheck);

  // Common options
  d->CommonPropertiesGroup = new QGroupBox(QStringLiteral("Common options"));
  d->PreserveTimeCheck = new QCheckBox(QStringLiteral("&Preserve timestamp"));
  d->PreserveTimeDirsCheck = new QCheckBox(QStringLiteral("Including directories"));
  d->PreserveTimeDirsCheck->setToolTip(QStringLiteral(
    "When preserving directory timestamps is enabled, using multiple connections for transfer is not possible."));
  d->CommonCalculateSizeCheck = new QCheckBox(QStringLiteral("&Calculate total size"));
  d->SpeedLabel = new QLabel(QStringLiteral("&Speed (KB/s):"));
  d->SpeedCombo = NewHistoryCombo(32767);
  d->SpeedCombo->addItems(DefaultSpeedLimits());
  d->SpeedLabel->setBuddy(d->SpeedCombo);
  auto * CommonLayout = new QGridLayout(d->CommonPropertiesGroup);
  CommonLayout->addWidget(d->PreserveTimeCheck, 0, 0, 1, 2);
  auto * DirsRow = new QHBoxLayout();
  DirsRow->setContentsMargins(18, 0, 0, 0);
  DirsRow->addWidget(d->PreserveTimeDirsCheck);
  CommonLayout->addLayout(DirsRow, 1, 0, 1, 2);
  CommonLayout->addWidget(d->CommonCalculateSizeCheck, 2, 0, 1, 2);
  CommonLayout->addWidget(d->SpeedLabel, 3, 0);
  CommonLayout->addWidget(d->SpeedCombo, 3, 1);
  CommonLayout->setRowStretch(4, 1);

  // Other
  d->OtherGroup = new QGroupBox(QStringLiteral("Other"));
  d->IncludeFileMaskLabel = new QLabel(QStringLiteral("File &mask:"));
  d->IncludeFileMaskCombo = NewHistoryCombo(3000);
  d->IncludeFileMaskLabel->setBuddy(d->IncludeFileMaskCombo);
  d->IncludeFileMaskButton = new QPushButton(QStringLiteral("&Edit..."));
  connect(d->IncludeFileMaskButton, &QPushButton::clicked, this, [this]()
  {
    // TCopyParamsFrame::IncludeFileMaskButtonClick
    QString Mask = d->IncludeFileMaskCombo->currentText();
    if (EditMaskDialog::EditMask(this, Mask))
    {
      d->IncludeFileMaskCombo->setEditText(Mask);
    }
  });
  auto * MaskHint = new QLabel(QStringLiteral("<a href=\"#\">mask hints</a>"));
  MaskHint->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
  MaskHint->setAlignment(Qt::AlignRight);
  QString HintText = EditMaskDialog::MaskHints(true, true);
  MaskHint->setToolTip(HintText);
  connect(MaskHint, &QLabel::linkActivated, this, [this, HintText]()
  {
    QMessageBox::information(this, window()->windowTitle(), HintText);
  });
  d->NewerOnlyCheck = new QCheckBox(QStringLiteral("&New and updated files only"));
  d->ExcludeHiddenFilesCheck = new QCheckBox(QStringLiteral("Exclude h&idden files"));
  d->ExcludeEmptyDirectoriesCheck = new QCheckBox(QStringLiteral("E&xclude empty directories"));
  auto * OtherLayout = new QGridLayout(d->OtherGroup);
  OtherLayout->addWidget(d->IncludeFileMaskLabel, 0, 0, 1, 2);
  auto * MaskRow = new QHBoxLayout();
  MaskRow->addWidget(d->IncludeFileMaskCombo, 1);
  MaskRow->addWidget(d->IncludeFileMaskButton);
  OtherLayout->addLayout(MaskRow, 1, 0, 1, 2);
  OtherLayout->addWidget(MaskHint, 2, 0, 1, 2);
  OtherLayout->addWidget(d->NewerOnlyCheck, 3, 0);
  OtherLayout->addWidget(d->ExcludeHiddenFilesCheck, 3, 1);
  OtherLayout->addWidget(d->ExcludeEmptyDirectoriesCheck, 4, 1);

  // forms/CopyParams.dfm layout
  auto * Grid = new QGridLayout(this);
  Grid->setContentsMargins(0, 0, 0, 0);
  Grid->addWidget(d->TransferModeGroup, 0, 0);
  Grid->addWidget(d->ChangeCaseGroup, 0, 1);
  Grid->addWidget(d->RemotePropertiesGroup, 1, 0, 2, 1);
  Grid->addWidget(d->CommonPropertiesGroup, 1, 1);
  Grid->addWidget(d->LocalPropertiesGroup, 2, 1);
  Grid->addWidget(d->OtherGroup, 3, 0, 1, 2);
  Grid->setColumnStretch(0, 273);
  Grid->setColumnStretch(1, 221);

  auto Update = [this]() { d->UpdateControls(); };
  for (QRadioButton * B : {d->TMTextButton, d->TMBinaryButton, d->TMAutomaticButton})
  {
    connect(B, &QRadioButton::toggled, this, Update);
  }
  for (QCheckBox * C : {d->PreserveRightsCheck, d->PreserveTimeCheck, d->IgnorePermErrorsCheck, d->RemoveCtrlZAndBOMCheck,
         d->NewerOnlyCheck, d->ExcludeHiddenFilesCheck, d->ExcludeEmptyDirectoriesCheck, d->PreserveTimeDirsCheck,
         d->PreserveReadOnlyCheck, d->CommonCalculateSizeCheck})
  {
    connect(C, &QCheckBox::toggled, this, Update);
  }
  connect(d->RightsEdit, &QLineEdit::editingFinished, this, [this]()
  {
    if (d->RightsEdit->isModified())
    {
      d->UpdateRightsByStr();
      d->RightsEdit->setModified(false);
    }
  });
  connect(d->SpeedCombo->lineEdit(), &QLineEdit::editingFinished, this, [this]()
  {
    unsigned long Speed = 0;
    if (TryGetSpeedLimit(d->SpeedCombo->currentText(), Speed))
    {
      d->SpeedCombo->setEditText(SetSpeedLimit(Speed));
    }
  });

  SetParams(CopyParam());
}

CopyParamsFrame::~CopyParamsFrame()
{
  delete d;
}

void CopyParamsFrame::SetCopyParamAttrs(int Value)
{
  d->CopyParamAttrs = Value;
  d->UpdateControls();
}

int CopyParamsFrame::CopyParamAttrs() const
{
  return d->CopyParamAttrs;
}

void CopyParamsFrame::SetParams(const CopyParam & Value)
{
  switch (Value.Mode)
  {
    case CopyParam::tmAscii: d->TMTextButton->setChecked(true); break;
    case CopyParam::tmBinary: d->TMBinaryButton->setChecked(true); break;
    default: d->TMAutomaticButton->setChecked(true); break;
  }
  d->AsciiFileMaskCombo->setEditText(Value.AsciiFileMask.Masks());
  d->AsciiFileMaskCombo->lineEdit()->setCursorPosition(0);

  switch (Value.FileNameCase)
  {
    case CopyParam::ncNoChange: d->CCNoChangeButton->setChecked(true); break;
    case CopyParam::ncLowerCase: d->CCLowerCaseButton->setChecked(true); break;
    case CopyParam::ncUpperCase: d->CCUpperCaseButton->setChecked(true); break;
    case CopyParam::ncFirstUpperCase: d->CCNoChangeButton->setChecked(true); break; // unsupported
    case CopyParam::ncLowerCaseShort: d->CCLowerCaseShortButton->setChecked(true); break;
  }

  d->Rights = Value.Rights;
  d->AddXToDirectoriesAction->setChecked(Value.AddXToDirectories);
  d->UpdateRightsControls();
  d->PreserveRightsCheck->setChecked(Value.PreserveRights);
  d->IgnorePermErrorsCheck->setChecked(Value.IgnorePermErrors);
  d->PreserveReadOnlyCheck->setChecked(Value.PreserveReadOnly);

  d->PreserveTimeCheck->setChecked(Value.PreserveTime);
  d->PreserveTimeDirsCheck->setChecked(Value.PreserveTimeDirs);
  d->CommonCalculateSizeCheck->setChecked(Value.CalculateSize);

  d->IncludeFileMaskCombo->setEditText(Value.IncludeFileMask.Masks());
  if (d->Attr(cpaNoRemoveCtrlZ))
  {
    d->RemoveCtrlZ = Value.RemoveCtrlZ;
    d->RemoveCtrlZAndBOMCheck->setChecked(Value.RemoveBOM);
  }
  else
  {
    d->RemoveCtrlZAndBOMCheck->setChecked(Value.RemoveCtrlZ && Value.RemoveBOM);
  }

  d->SpeedCombo->setEditText(SetSpeedLimit(Value.CPSLimit));

  d->NewerOnlyCheck->setChecked(Value.NewerOnly);
  d->ExcludeHiddenFilesCheck->setChecked(Value.ExcludeHiddenFiles);
  d->ExcludeEmptyDirectoriesCheck->setChecked(Value.ExcludeEmptyDirectories);

  d->Params = Value;
  d->UpdateControls();
}

CopyParam CopyParamsFrame::Params() const
{
  // TCopyParamsFrame::GetParams
  CopyParam Result = d->Params;
  if (d->TMTextButton->isChecked()) Result.Mode = CopyParam::tmAscii;
  else if (d->TMBinaryButton->isChecked()) Result.Mode = CopyParam::tmBinary;
  else Result.Mode = CopyParam::tmAutomatic;

  try
  {
    Result.AsciiFileMask = d->AsciiFileMaskCombo->currentText();
  }
  catch (const FileMasksException &)
  {
  }

  if (d->CCLowerCaseButton->isChecked()) Result.FileNameCase = CopyParam::ncLowerCase;
  else if (d->CCUpperCaseButton->isChecked()) Result.FileNameCase = CopyParam::ncUpperCase;
  else if (d->CCLowerCaseShortButton->isChecked()) Result.FileNameCase = CopyParam::ncLowerCaseShort;
  else Result.FileNameCase = CopyParam::ncNoChange;

  Result.AddXToDirectories = d->AddXToDirectoriesAction->isChecked();
  unsigned int Mode = d->Rights;
  QString RightsText = d->RightsEdit->text().trimmed();
  if (RightsFromText(RightsText, Mode) || Rights::FromOctal(RightsText, Mode))
  {
    Result.Rights = Mode;
  }
  else
  {
    Result.Rights = d->Rights;
  }
  Result.PreserveRights = d->PreserveRightsCheck->isChecked();
  Result.IgnorePermErrors = d->IgnorePermErrorsCheck->isChecked();
  Result.PreserveReadOnly = d->PreserveReadOnlyCheck->isChecked();

  Result.PreserveTime = d->PreserveTimeCheck->isChecked();
  Result.PreserveTimeDirs = d->PreserveTimeDirsCheck->isChecked();
  Result.CalculateSize = d->CommonCalculateSizeCheck->isChecked();

  try
  {
    Result.IncludeFileMask = d->IncludeFileMaskCombo->currentText();
  }
  catch (const FileMasksException &)
  {
  }

  Result.RemoveCtrlZ = d->Attr(cpaNoRemoveCtrlZ) ? d->RemoveCtrlZ : d->RemoveCtrlZAndBOMCheck->isChecked();
  Result.RemoveBOM = d->RemoveCtrlZAndBOMCheck->isChecked();

  unsigned long Speed = 0;
  if (TryGetSpeedLimit(d->SpeedCombo->currentText(), Speed))
  {
    Result.CPSLimit = Speed;
  }

  Result.NewerOnly = d->NewerOnlyCheck->isChecked();
  Result.ExcludeHiddenFiles = d->ExcludeHiddenFilesCheck->isChecked();
  Result.ExcludeEmptyDirectories = d->ExcludeEmptyDirectoriesCheck->isChecked();
  return Result;
}

bool CopyParamsFrame::Validate()
{
  // ValidateMaskComboExit / RightsEditExit / SpeedComboExit
  for (QComboBox * Combo : {d->AsciiFileMaskCombo, d->IncludeFileMaskCombo})
  {
    if (!Combo->isEnabled())
    {
      continue;
    }
    if (!EditMaskDialog::ValidateMask(this, Combo->currentText(), Combo))
    {
      return false;
    }
  }
  if (d->RightsEdit->isEnabled() && !d->UpdateRightsByStr())
  {
    QMessageBox::critical(this, window()->windowTitle(),
      QStringLiteral("'%1' is not valid permission in octal format.").arg(d->RightsEdit->text()));
    d->RightsEdit->setFocus();
    d->RightsEdit->selectAll();
    return false;
  }
  try
  {
    GetSpeedLimit(d->SpeedCombo->currentText());
  }
  catch (const ExtException & E)
  {
    QMessageBox::critical(this, window()->windowTitle(), E.Message());
    d->SpeedCombo->setFocus();
    d->SpeedCombo->lineEdit()->selectAll();
    return false;
  }
  return true;
}

void CopyParamsFrame::BeforeExecute()
{
  EditMaskDialog::LoadHistoryCombo(d->AsciiFileMaskCombo, QStringLiteral("Mask"));
  EditMaskDialog::LoadHistoryCombo(d->IncludeFileMaskCombo, QStringLiteral("IncludeMask"));
  // CopySpeedLimits: the remembered limits, completed by the standard ones
  QString Text = d->SpeedCombo->currentText();
  QStringList Items;
  for (const QString & Item : EditMaskDialog::History(QStringLiteral("SpeedLimit")))
  {
    unsigned long Speed = 0;
    if (TryGetSpeedLimit(Item, Speed) && !Items.contains(SetSpeedLimit(Speed)))
    {
      Items.append(SetSpeedLimit(Speed));
    }
  }
  if (Items.isEmpty())
  {
    Items = DefaultSpeedLimits();
  }
  d->SpeedCombo->clear();
  d->SpeedCombo->addItems(Items);
  d->SpeedCombo->setEditText(Text);
}

void CopyParamsFrame::AfterExecute()
{
  if (!d->AsciiFileMaskCombo->currentText().isEmpty())
  {
    EditMaskDialog::SaveToHistory(QStringLiteral("Mask"), d->AsciiFileMaskCombo->currentText());
  }
  if (!d->IncludeFileMaskCombo->currentText().isEmpty())
  {
    EditMaskDialog::SaveToHistory(QStringLiteral("IncludeMask"), d->IncludeFileMaskCombo->currentText());
  }
  // History["SpeedLimit"] = SpeedCombo->Items (after SaveToHistory)
  QStringList Items;
  unsigned long Speed = 0;
  if (TryGetSpeedLimit(d->SpeedCombo->currentText(), Speed))
  {
    Items.append(SetSpeedLimit(Speed));
  }
  for (int Index = 0; Index < d->SpeedCombo->count(); Index++)
  {
    if (!Items.contains(d->SpeedCombo->itemText(Index)))
    {
      Items.append(d->SpeedCombo->itemText(Index));
    }
  }
  for (int Index = Items.size() - 1; Index >= 0; Index--)
  {
    EditMaskDialog::SaveToHistory(QStringLiteral("SpeedLimit"), Items.at(Index));
  }
}

//---------------------------------------------------------------------------
// CopyParamsDialog
//---------------------------------------------------------------------------

CopyParamsDialog::CopyParamsDialog(const CopyParam & Param, int CopyParamAttrs, QWidget * Parent) :
  QDialog(Parent)
{
  setWindowTitle(QStringLiteral("Transfer settings"));
  FFrame = new CopyParamsFrame();
  FFrame->SetCopyParamAttrs(CopyParamAttrs);
  FFrame->SetParams(Param);
  FFrame->BeforeExecute();

  auto * Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  HelpSystem::AddHelpButton(Buttons, this, QStringLiteral("ui_transfer_custom"));
  connect(Buttons, &QDialogButtonBox::accepted, this, &CopyParamsDialog::accept);
  connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  auto * Layout = new QVBoxLayout(this);
  Layout->addWidget(FFrame);
  Layout->addWidget(Buttons);
  FFrame->setFocus();
}

CopyParam CopyParamsDialog::Param() const
{
  return FFrame->Params();
}

void CopyParamsDialog::accept()
{
  if (FFrame->Validate())
  {
    FFrame->AfterExecute();
    QDialog::accept();
  }
}

bool CopyParamsDialog::Execute(CopyParam & Param, int CopyParamAttrs, QWidget * Parent)
{
  CopyParamsDialog Dialog(Param, CopyParamAttrs, Parent);
  if (Dialog.exec() == QDialog::Accepted)
  {
    Param = Dialog.Param();
    return true;
  }
  return false;
}

//---------------------------------------------------------------------------
// CopyParamPresetDialog
//---------------------------------------------------------------------------

struct CopyParamPresetDialog::Private
{
  Mode AMode = cpmAdd;
  const CopyParamRuleData * CurrentRuleData = nullptr;
  CopyParamList * List = nullptr;
  int Index = -1;

  QLineEdit * DescriptionEdit = nullptr;
  CopyParamsFrame * Frame = nullptr;
  QCheckBox * HasRuleCheck = nullptr;
  QGroupBox * RuleGroup = nullptr;
  QLineEdit * HostNameEdit = nullptr;
  QLineEdit * UserNameEdit = nullptr;
  QLineEdit * RemoteDirectoryEdit = nullptr;
  QLineEdit * LocalDirectoryEdit = nullptr;
  QPushButton * CurrentRuleButton = nullptr;
  QPushButton * OkButton = nullptr;

  void UpdateControls();
  void SetRuleData(const CopyParamRuleData & Data);
  CopyParamRuleData RuleData() const;
};

void CopyParamPresetDialog::Private::UpdateControls()
{
  OkButton->setEnabled(!DescriptionEdit->text().isEmpty());
  RuleGroup->setEnabled(HasRuleCheck->isChecked());
  // current rule data are loaded implicitly with cpmAddCurrent
  CurrentRuleButton->setVisible((CurrentRuleData != nullptr) && (AMode != cpmAddCurrent));
}

void CopyParamPresetDialog::Private::SetRuleData(const CopyParamRuleData & Data)
{
  HostNameEdit->setText(Data.HostName);
  UserNameEdit->setText(Data.UserName);
  RemoteDirectoryEdit->setText(Data.RemoteDirectory);
  LocalDirectoryEdit->setText(Data.LocalDirectory);
}

CopyParamRuleData CopyParamPresetDialog::Private::RuleData() const
{
  CopyParamRuleData Data;
  Data.HostName = HostNameEdit->text();
  Data.UserName = UserNameEdit->text();
  Data.RemoteDirectory = RemoteDirectoryEdit->text();
  Data.LocalDirectory = LocalDirectoryEdit->text();
  return Data;
}

CopyParamPresetDialog::CopyParamPresetDialog(QWidget * Parent, Mode AMode, const CopyParamRuleData * CurrentRuleData) :
  QDialog(Parent),
  d(new Private)
{
  d->AMode = AMode;
  d->CurrentRuleData = CurrentRuleData;
  // COPY_PARAM_EDIT / COPY_PARAM_ADD
  setWindowTitle((AMode == cpmEdit) ? QStringLiteral("Edit transfer settings preset") :
    QStringLiteral("Add transfer settings preset"));

  auto * DescriptionLabel = new QLabel(QStringLiteral("Preset &description:"));
  d->DescriptionEdit = new QLineEdit();
  d->DescriptionEdit->setMaxLength(250);
  DescriptionLabel->setBuddy(d->DescriptionEdit);

  d->Frame = new CopyParamsFrame();

  d->HasRuleCheck = new QCheckBox(QStringLiteral("Automatically select this preset when"));
  d->RuleGroup = new QGroupBox(QStringLiteral("Autoselection rule"));
  auto NewMaskEdit = [](const QString & Caption, QLineEdit *& Edit, QVBoxLayout * Layout)
  {
    auto * Label = new QLabel(Caption);
    Edit = new QLineEdit();
    Edit->setMaxLength(250);
    Label->setBuddy(Edit);
    Layout->addWidget(Label);
    Layout->addWidget(Edit);
  };
  auto * RuleLayout = new QVBoxLayout(d->RuleGroup);
  NewMaskEdit(QStringLiteral("Hostna&me mask:"), d->HostNameEdit, RuleLayout);
  NewMaskEdit(QStringLiteral("Us&ername mask:"), d->UserNameEdit, RuleLayout);
  NewMaskEdit(QStringLiteral("Remote director&y mask:"), d->RemoteDirectoryEdit, RuleLayout);
  NewMaskEdit(QStringLiteral("&Local directory mask:"), d->LocalDirectoryEdit, RuleLayout);
  d->CurrentRuleButton = new QPushButton(QStringLiteral("Current"));
  auto * CurrentRow = new QHBoxLayout();
  CurrentRow->addWidget(d->CurrentRuleButton);
  CurrentRow->addStretch();
  RuleLayout->addLayout(CurrentRow);
  auto * RuleMaskHint = new QLabel(QStringLiteral("<a href=\"#\">mask hints</a>"));
  RuleMaskHint->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
  RuleMaskHint->setAlignment(Qt::AlignRight);
  QString HintText = EditMaskDialog::MaskHints(false, false);
  RuleMaskHint->setToolTip(HintText);
  connect(RuleMaskHint, &QLabel::linkActivated, this, [this, HintText]()
  {
    QMessageBox::information(this, windowTitle(), HintText);
  });
  RuleLayout->addWidget(RuleMaskHint);
  RuleLayout->addStretch();
  connect(d->CurrentRuleButton, &QPushButton::clicked, this, [this]()
  {
    if (d->CurrentRuleData != nullptr)
    {
      d->SetRuleData(*d->CurrentRuleData);
    }
  });

  auto * Buttons = new QDialogButtonBox();
  d->OkButton = Buttons->addButton(QDialogButtonBox::Ok);
  Buttons->addButton(QDialogButtonBox::Cancel);
  HelpSystem::AddHelpButton(Buttons, this, QStringLiteral("ui_transfer_preset"));
  connect(Buttons, &QDialogButtonBox::accepted, this, &CopyParamPresetDialog::accept);
  connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  auto * Left = new QVBoxLayout();
  Left->addWidget(DescriptionLabel);
  Left->addWidget(d->DescriptionEdit);
  Left->addWidget(d->Frame, 1);
  auto * Right = new QVBoxLayout();
  Right->addWidget(d->HasRuleCheck);
  Right->addWidget(d->RuleGroup, 1);
  auto * Columns = new QHBoxLayout();
  Columns->addLayout(Left, 456);
  Columns->addLayout(Right, 264);
  auto * Layout = new QVBoxLayout(this);
  Layout->addLayout(Columns);
  Layout->addWidget(Buttons);

  connect(d->DescriptionEdit, &QLineEdit::textChanged, this, [this]() { d->UpdateControls(); });
  connect(d->HasRuleCheck, &QCheckBox::toggled, this, [this]() { d->UpdateControls(); });
}

CopyParamPresetDialog::~CopyParamPresetDialog()
{
  delete d;
}

void CopyParamPresetDialog::accept()
{
  // TCopyParamPresetDialog::FormCloseQuery
  if (!d->Frame->Validate())
  {
    return;
  }
  QString Description = d->DescriptionEdit->text();
  try
  {
    CopyParamList::ValidateName(Description);
  }
  catch (const ExtException & E)
  {
    QMessageBox::critical(this, windowTitle(), E.Message());
    d->DescriptionEdit->setFocus();
    return;
  }
  if (d->HasRuleCheck->isChecked())
  {
    for (QLineEdit * Edit : {d->HostNameEdit, d->UserNameEdit, d->RemoteDirectoryEdit, d->LocalDirectoryEdit})
    {
      if (!EditMaskDialog::ValidateMask(this, Edit->text()))
      {
        Edit->setFocus();
        Edit->selectAll();
        return;
      }
    }
    if (CopyParamRule(d->RuleData()).IsEmpty())
    {
      QMessageBox::critical(this, windowTitle(), QStringLiteral("You have not specified any autoselection rule mask."));
      d->HostNameEdit->setFocus();
      return;
    }
  }
  int Index = d->List->IndexOfName(Description);
  if (((d->AMode == cpmEdit) && (Index >= 0) && (Index != d->Index)) ||
      ((d->AMode != cpmEdit) && (Index >= 0)))
  {
    // COPY_PARAM_DUPLICATE
    QMessageBox::critical(this, windowTitle(),
      QStringLiteral("Transfer settings preset with description '%1' already exists.").arg(Description));
    d->DescriptionEdit->setFocus();
    return;
  }
  QDialog::accept();
}

bool CopyParamPresetDialog::Execute(QWidget * Parent, CopyParamList & List, int & Index, Mode AMode,
  const CopyParamRuleData * CurrentRuleData, const CopyParam & DefaultCopyParams)
{
  CopyParamPresetDialog Dialog(Parent, AMode, CurrentRuleData);
  Dialog.d->List = &List;
  if ((AMode == cpmEdit) || (AMode == cpmDuplicate))
  {
    const CopyParamRule * Rule = nullptr;
    if (Index >= 0)
    {
      Dialog.d->Frame->SetParams(List.Param(Index));
      Rule = List.Rule(Index);
    }
    else
    {
      Dialog.d->Frame->SetParams(DefaultCopyParams);
    }
    if (AMode == cpmEdit)
    {
      Dialog.d->DescriptionEdit->setText(List.Name(Index));
      Dialog.d->Index = Index;
    }
    else
    {
      Dialog.d->Index = -1;
      Index = List.Count();
    }
    Dialog.d->HasRuleCheck->setChecked(Rule != nullptr);
    if (Rule != nullptr)
    {
      Dialog.d->SetRuleData(Rule->Data);
    }
  }
  else
  {
    Dialog.d->Frame->SetParams(DefaultCopyParams);
    if ((AMode == cpmAddCurrent) && (CurrentRuleData != nullptr))
    {
      Dialog.d->SetRuleData(*CurrentRuleData);
      Dialog.d->HasRuleCheck->setChecked(true);
    }
    Dialog.d->Index = -1;
    if (Index < 0)
    {
      Index = List.Count();
    }
  }
  Dialog.d->Frame->BeforeExecute();
  Dialog.d->UpdateControls();
  Dialog.d->DescriptionEdit->setFocus();

  if (Dialog.exec() != QDialog::Accepted)
  {
    return false;
  }
  Dialog.d->Frame->AfterExecute();
  QString Name = Dialog.d->DescriptionEdit->text();
  CopyParam Param = Dialog.d->Frame->Params();
  std::optional<CopyParamRule> Rule;
  if (Dialog.d->HasRuleCheck->isChecked())
  {
    Rule = CopyParamRule(Dialog.d->RuleData());
  }
  if (AMode == cpmEdit)
  {
    List.Change(Index, Name, Param, Rule);
  }
  else
  {
    List.Insert(Index, Name, Param, Rule);
  }
  return true;
}

//---------------------------------------------------------------------------
// CopyParamListFrame
//---------------------------------------------------------------------------

struct CopyParamListFrame::Private : public QObject
{
  CopyParamListFrame * Frame = nullptr;
  CopyParam * DefaultParam = nullptr;
  CopyParamList List;
  const CopyParamRuleData * CurrentRuleData = nullptr;

  QLabel * CopyParamLabel = nullptr;
  QTreeWidget * CopyParamListView = nullptr;
  QPushButton * AddCopyParamButton = nullptr;
  QPushButton * RemoveCopyParamButton = nullptr;
  QPushButton * UpCopyParamButton = nullptr;
  QPushButton * DownCopyParamButton = nullptr;
  QPushButton * EditCopyParamButton = nullptr;
  QPushButton * DuplicateCopyParamButton = nullptr;
  QCheckBox * CopyParamAutoSelectNoticeCheck = nullptr;

  int ItemIndex() const;
  void SetItemIndex(int Index);
  void UpdateCopyParamListView();
  void AddEditCopyParam(CopyParamPresetDialog::Mode AMode);
  void CopyParamMove(int Source, int Dest);
  void Remove();
  bool eventFilter(QObject * Watched, QEvent * Event) override;
};

int CopyParamListFrame::Private::ItemIndex() const
{
  return CopyParamListView->indexOfTopLevelItem(CopyParamListView->currentItem());
}

void CopyParamListFrame::Private::SetItemIndex(int Index)
{
  QTreeWidgetItem * Item = CopyParamListView->topLevelItem(Index);
  CopyParamListView->setCurrentItem(Item);
}

void CopyParamListFrame::Private::UpdateCopyParamListView()
{
  // TPreferencesDialog::UpdateCopyParamListView / CopyParamListViewData
  int Index = ItemIndex();
  CopyParamListView->clear();
  auto * DefaultItem = new QTreeWidgetItem(QStringList() << QStringLiteral("Default"));   // StripHotkey(COPY_PARAM_DEFAULT)
  QFont Font = DefaultItem->font(0);
  Font.setBold(true);
  DefaultItem->setFont(0, Font);
  CopyParamListView->addTopLevelItem(DefaultItem);
  for (int I = 0; I < List.Count(); I++)
  {
    QString Name = List.Name(I);
    Name.remove(QLatin1Char('&'));
    // BooleanToStr
    CopyParamListView->addTopLevelItem(new QTreeWidgetItem(QStringList() << Name <<
      ((List.Rule(I) != nullptr) ? QStringLiteral("Yes") : QStringLiteral("No"))));
  }
  CopyParamListView->resizeColumnToContents(1);
  SetItemIndex(qBound(0, Index, List.Count()));
}

void CopyParamListFrame::Private::AddEditCopyParam(CopyParamPresetDialog::Mode AMode)
{
  // TPreferencesDialog::AddEditCopyParam
  int Index = ItemIndex();
  bool Result;
  if ((Index == 0) && (AMode == CopyParamPresetDialog::cpmEdit))
  {
    Result = CopyParamsDialog::Execute(*DefaultParam, 0, Frame);
  }
  else
  {
    // negative (when default is selected) means add to the end
    Index--;
    CopyParam DefaultCopyParams;   // for cpmAdd use defaults
    if (AMode == CopyParamPresetDialog::cpmDuplicate)
    {
      // only used, when duplicating default settings (Index < 0)
      DefaultCopyParams = *DefaultParam;
    }
    if (AMode == CopyParamPresetDialog::cpmAdd)
    {
      Index = -1;
    }
    Result = CopyParamPresetDialog::Execute(Frame, List, Index, AMode, CurrentRuleData, DefaultCopyParams);
    if (Result)
    {
      UpdateCopyParamListView();
      SetItemIndex(Index + 1);
    }
  }
  if (Result)
  {
    Frame->UpdateControls();
    emit Frame->Changed();
  }
}

void CopyParamListFrame::Private::CopyParamMove(int Source, int Dest)
{
  if ((Source > 0) && (Dest > 0) && (Dest <= List.Count()))
  {
    List.Move(Source - 1, Dest - 1);
    UpdateCopyParamListView();
    SetItemIndex(Dest);
    Frame->UpdateControls();
    emit Frame->Changed();
  }
}

void CopyParamListFrame::Private::Remove()
{
  int Index = ItemIndex();
  if ((Index >= 1) && (Index <= List.Count()))
  {
    List.Delete(Index - 1);
    UpdateCopyParamListView();
    Frame->UpdateControls();
    emit Frame->Changed();
  }
}

bool CopyParamListFrame::Private::eventFilter(QObject * Watched, QEvent * Event)
{
  if ((Watched == CopyParamLabel) && (Event->type() == QEvent::MouseButtonRelease))
  {
    // TPreferencesDialog::CopyParamLabelClick
    if (EditCopyParamButton->isEnabled())
    {
      AddEditCopyParam(CopyParamPresetDialog::cpmEdit);
    }
    return true;
  }
  if ((Watched == CopyParamListView) && (Event->type() == QEvent::KeyPress))
  {
    // TPreferencesDialog::CopyParamListViewKeyDown
    int Key = static_cast<QKeyEvent *>(Event)->key();
    if ((Key == Qt::Key_Delete) && RemoveCopyParamButton->isEnabled())
    {
      Remove();
      return true;
    }
    if ((Key == Qt::Key_Insert) && AddCopyParamButton->isEnabled())
    {
      AddEditCopyParam(CopyParamPresetDialog::cpmAdd);
      return true;
    }
  }
  return QObject::eventFilter(Watched, Event);
}

CopyParamListFrame::CopyParamListFrame(CopyParam * DefaultParam, QWidget * Parent) :
  QWidget(Parent),
  d(new Private)
{
  d->Frame = this;
  d->DefaultParam = DefaultParam;

  auto * Group = new QGroupBox(QStringLiteral("Transfer settings presets"));
  d->CopyParamLabel = new QLabel();
  d->CopyParamLabel->setWordWrap(true);
  d->CopyParamLabel->setTextFormat(Qt::PlainText);
  d->CopyParamLabel->setFrameShape(QFrame::StyledPanel);
  d->CopyParamLabel->setMargin(6);
  d->CopyParamLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  d->CopyParamLabel->setMinimumHeight(d->CopyParamLabel->fontMetrics().lineSpacing() * 3 + 12);
  d->CopyParamLabel->setCursor(Qt::PointingHandCursor);
  d->CopyParamLabel->installEventFilter(d);

  d->CopyParamListView = new QTreeWidget();
  d->CopyParamListView->setColumnCount(2);
  d->CopyParamListView->setHeaderLabels({QStringLiteral("Preset description"), QStringLiteral("Auto")});
  d->CopyParamListView->setRootIsDecorated(false);
  d->CopyParamListView->setUniformRowHeights(true);
  d->CopyParamListView->setAllColumnsShowFocus(true);
  d->CopyParamListView->header()->setStretchLastSection(false);
  d->CopyParamListView->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  d->CopyParamListView->header()->setSectionsClickable(false);
  d->CopyParamListView->installEventFilter(d);

  d->AddCopyParamButton = new QPushButton(QStringLiteral("&Add..."));
  d->EditCopyParamButton = new QPushButton(QStringLiteral("&Edit..."));
  d->DuplicateCopyParamButton = new QPushButton(QStringLiteral("Du&plicate..."));
  d->RemoveCopyParamButton = new QPushButton(QStringLiteral("&Remove"));
  d->UpCopyParamButton = new QPushButton(QStringLiteral("&Up"));
  d->DownCopyParamButton = new QPushButton(QStringLiteral("&Down"));
  auto * ButtonColumn = new QVBoxLayout();
  for (QPushButton * B : {d->AddCopyParamButton, d->EditCopyParamButton, d->DuplicateCopyParamButton,
         d->RemoveCopyParamButton, d->UpCopyParamButton, d->DownCopyParamButton})
  {
    ButtonColumn->addWidget(B);
  }
  ButtonColumn->addStretch();

  d->CopyParamAutoSelectNoticeCheck = new QCheckBox(QStringLiteral("&Announce when transfer settings preset is autoselected"));

  auto * ListRow = new QHBoxLayout();
  ListRow->addWidget(d->CopyParamListView, 1);
  ListRow->addLayout(ButtonColumn);
  auto * GroupLayout = new QVBoxLayout(Group);
  GroupLayout->addWidget(d->CopyParamLabel);
  GroupLayout->addLayout(ListRow, 1);
  GroupLayout->addWidget(d->CopyParamAutoSelectNoticeCheck);
  auto * Layout = new QVBoxLayout(this);
  Layout->setContentsMargins(0, 0, 0, 0);
  Layout->addWidget(Group);

  connect(d->AddCopyParamButton, &QPushButton::clicked, this, [this]() { d->AddEditCopyParam(CopyParamPresetDialog::cpmAdd); });
  connect(d->EditCopyParamButton, &QPushButton::clicked, this, [this]() { d->AddEditCopyParam(CopyParamPresetDialog::cpmEdit); });
  connect(d->DuplicateCopyParamButton, &QPushButton::clicked, this, [this]() { d->AddEditCopyParam(CopyParamPresetDialog::cpmDuplicate); });
  connect(d->RemoveCopyParamButton, &QPushButton::clicked, this, [this]() { d->Remove(); });
  connect(d->UpCopyParamButton, &QPushButton::clicked, this, [this]() { d->CopyParamMove(d->ItemIndex(), d->ItemIndex() - 1); });
  connect(d->DownCopyParamButton, &QPushButton::clicked, this, [this]() { d->CopyParamMove(d->ItemIndex(), d->ItemIndex() + 1); });
  connect(d->CopyParamListView, &QTreeWidget::itemDoubleClicked, this, [this]()
  {
    if (d->EditCopyParamButton->isEnabled())
    {
      d->AddEditCopyParam(CopyParamPresetDialog::cpmEdit);
    }
  });
  connect(d->CopyParamListView, &QTreeWidget::currentItemChanged, this, [this]() { UpdateControls(); });

  d->UpdateCopyParamListView();
  UpdateControls();
  d->setParent(this);
}

CopyParamListFrame::~CopyParamListFrame()
{
  // d is a child QObject
}

void CopyParamListFrame::SetList(const CopyParamList & List)
{
  d->List = List;
  d->UpdateCopyParamListView();
  d->SetItemIndex(0);
  UpdateControls();
}

CopyParamList CopyParamListFrame::List() const
{
  return d->List;
}

void CopyParamListFrame::SetCurrentRuleData(const CopyParamRuleData * Data)
{
  d->CurrentRuleData = Data;
}

QCheckBox * CopyParamListFrame::AutoSelectNoticeCheck() const
{
  return d->CopyParamAutoSelectNoticeCheck;
}

void CopyParamListFrame::UpdateControls()
{
  // TPreferencesDialog::UpdateControls (presets part)
  int Index = d->ItemIndex();
  bool CopyParamSelected = (Index >= 0);
  d->EditCopyParamButton->setEnabled(CopyParamSelected);
  d->DuplicateCopyParamButton->setEnabled(CopyParamSelected);
  d->RemoveCopyParamButton->setEnabled(CopyParamSelected && (Index >= 1));
  d->UpCopyParamButton->setEnabled(CopyParamSelected && (Index > 1));
  d->DownCopyParamButton->setEnabled(CopyParamSelected && (Index >= 1) && (Index < d->CopyParamListView->topLevelItemCount() - 1));
  d->CopyParamAutoSelectNoticeCheck->setEnabled(d->List.AnyRule());

  QString InfoStr;
  if (CopyParamSelected)
  {
    const CopyParam & Selected = (Index == 0) ? *d->DefaultParam : d->List.Param(Index - 1);
    InfoStr = Selected.GetInfoStr(QStringLiteral("; "), 0);
    if (Index >= 1)
    {
      const CopyParamRule * Rule = d->List.Rule(Index - 1);
      if (Rule != nullptr)
      {
        // COPY_PARAM_RULE (with the line break replaced by space)
        InfoStr += QStringLiteral("\nAutoselection rule: %1").arg(Rule->GetInfoStr(QStringLiteral("; ")));
      }
    }
  }
  d->CopyParamLabel->setText(InfoStr);
  d->CopyParamLabel->setToolTip(InfoStr);
}

void CopyParamListFrame::showEvent(QShowEvent * Event)
{
  // the default transfer settings may have been changed on other pages
  UpdateControls();
  QWidget::showEvent(Event);
}

//---------------------------------------------------------------------------
// Presets menu
//---------------------------------------------------------------------------

int CopyParamListPopup(QWidget * Parent, const QPoint & GlobalPos, CopyParam & Param, QString & Preset,
  int Options, int CopyParamAttrs, bool * SaveSettings)
{
  enum { cpiDefault = -1, cpiConfigure = -2, cpiCustom = -3, cpiSaveSettings = -4, cpiSavePreset = -6 };
  CopyParamPresets & Presets = CopyParamPresets::Instance();

  QMenu Menu(Parent);
  QAction * CustomizeItem = Menu.addAction(QStringLiteral("Custo&m..."));   // COPY_PARAM_CUSTOM
  CustomizeItem->setData(int(cpiCustom));
  CustomizeItem->setCheckable(true);
  if ((Options & cplCustomizeDefault) != 0)
  {
    QFont Font = CustomizeItem->font();
    Font.setBold(true);
    CustomizeItem->setFont(Font);
  }

  if (((Options & cplSaveSettings) != 0) && (SaveSettings != nullptr))
  {
    QAction * Item = Menu.addAction(QStringLiteral("&Set as Default"));   // COPY_PARAM_SAVE_SETTINGS
    Item->setData(int(cpiSaveSettings));
    Item->setCheckable(true);
    Item->setChecked(*SaveSettings);
  }

  QAction * SavePresetItem = Menu.addAction(QStringLiteral("Save as &Preset..."));   // COPY_PARAM_SAVE_PRESET
  SavePresetItem->setData(int(cpiSavePreset));

  bool AnyChecked = false;
  QAction * DefaultItem = Menu.addAction(QStringLiteral("&Default"));   // COPY_PARAM_DEFAULT
  DefaultItem->setData(int(cpiDefault));
  DefaultItem->setCheckable(true);
  DefaultItem->setChecked(Preset.isEmpty() && (Presets.Preset(QString()) == Param));
  AnyChecked = AnyChecked || DefaultItem->isChecked();

  CopyParam DefaultParam;
  const CopyParamList & List = Presets.List();
  for (int Index = 0; Index < List.Count(); Index++)
  {
    QString Name = List.Name(Index);
    CopyParam AParam = Presets.Preset(Name);
    if (AParam.AnyUsableCopyParam(CopyParamAttrs) ||
        // This makes "Binary" preset visible, as long as we care about transfer mode
        ((AParam == DefaultParam) &&
         ((CopyParamAttrs & cpaIncludeMaskOnly) == 0) && ((CopyParamAttrs & cpaNoTransferMode) == 0)))
    {
      QAction * Item = Menu.addAction(Name);
      Item->setData(Index);
      Item->setCheckable(true);
      Item->setChecked((Preset == Name) && (AParam == Param));
      AnyChecked = AnyChecked || Item->isChecked();
    }
  }
  CustomizeItem->setChecked(!AnyChecked);

  Menu.addSeparator();
  QAction * ConfigureItem = Menu.addAction(QStringLiteral("&Configure..."));   // COPY_PARAM_CONFIGURE
  ConfigureItem->setData(int(cpiConfigure));

  QAction * Chosen = Menu.exec(GlobalPos);
  if (Chosen == nullptr)
  {
    return 0;
  }

  // CopyParamListPopupClick
  int Tag = Chosen->data().toInt();
  int Result = 0;
  if (Tag == cpiConfigure)
  {
    bool MatchedPreset = (Presets.Preset(Preset) == Param);
    PreferencesDialog::Execute(Parent, QStringLiteral("Transfer"));
    Result = (MatchedPreset && Presets.HasPreset(Preset)) ? 1 : 0;
    if (Result > 0)
    {
      Param = Presets.Preset(Preset);
    }
  }
  else if (Tag == cpiCustom)
  {
    Result = CopyParamsDialog::Execute(Param, CopyParamAttrs, Parent) ? 1 : 0;
  }
  else if (Tag == cpiSaveSettings)
  {
    if (SaveSettings != nullptr)
    {
      *SaveSettings = !*SaveSettings;
    }
  }
  else if (Tag == cpiSavePreset)
  {
    CopyParamList NewList = Presets.List();
    int Index = -1;
    if (CopyParamPresetDialog::Execute(Parent, NewList, Index, CopyParamPresetDialog::cpmAdd, nullptr, Param))
    {
      Presets.SetList(NewList);
      Configuration::Instance().Save();
      // If saved unmodified, then make this the selected preset
      if (NewList.Param(Index) == Param)
      {
        Preset = NewList.Name(Index);
      }
    }
  }
  else
  {
    Preset = (Tag >= 0) ? List.Name(Tag) : QString();
    // the queue options are not configurable in presets: keep the current ones
    CopyParam APreset = Presets.Preset(Preset);
    APreset.Queue = Param.Queue;
    APreset.QueueNoConfirmation = Param.QueueNoConfirmation;
    APreset.QueueParallel = Param.QueueParallel;
    APreset.FileMask = Param.FileMask;
    Param = APreset;
    Result = 1;
  }
  return Result;
}
