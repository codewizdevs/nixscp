// Port of WinSCP source/forms/Properties.{cpp,dfm}, forms/Rights.{cpp,dfm}
// and forms/CreateDirectory.{cpp,dfm}
#include "gui/PropertiesDialog.h"
#include "gui/HelpSystem.h"

#include "core/Configuration.h"
#include "core/CopyParam.h"
#include "core/IniStorage.h"
#include "gui/GuiUserInterface.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QSignalBlocker>
#include <QStyle>
#include <QTabWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cmath>

//---------------------------------------------------------------------------
// RightsWidget  (TRightsFrame)
//---------------------------------------------------------------------------

// The pimpls derive from QObject so that they are deleted together with
// their widget (the header declares no destructors). They are re-parented
// at the end of each constructor, to be the last child destroyed.
struct RightsWidget::Private : public QObject
{
  // [Owner, Group, Others][Read, Write, Execute]
  QCheckBox * Checks[3][3] = {};
  QCheckBox * SetUidCheck = nullptr;
  QCheckBox * SetGidCheck = nullptr;
  QCheckBox * StickyBitCheck = nullptr;
  QLineEdit * OctalEdit = nullptr;
  QCheckBox * DirectoriesXCheck = nullptr;
  bool AllowUndef = false;
  bool AddXVisible = false;

  // Every check box with its bit
  QList<std::pair<QCheckBox *, unsigned int>> AllChecks() const;
  unsigned int Mask(Qt::CheckState State) const;
  void UpdateChecks(unsigned int Set, unsigned int Unset);
  void UpdateOctalEdit();
  void UpdateControls();
};

QList<std::pair<QCheckBox *, unsigned int>> RightsWidget::Private::AllChecks() const
{
  QList<std::pair<QCheckBox *, unsigned int>> Result;
  for (int Who = 0; Who < 3; Who++)
  {
    for (int What = 0; What < 3; What++)
    {
      Result.append({Checks[Who][What], 1u << (8 - (Who * 3 + What))});
    }
  }
  Result.append({SetUidCheck, 04000u});
  Result.append({SetGidCheck, 02000u});
  Result.append({StickyBitCheck, 01000u});
  return Result;
}

unsigned int RightsWidget::Private::Mask(Qt::CheckState State) const
{
  unsigned int Result = 0;
  for (const auto & [Check, Bit] : AllChecks())
  {
    if (Check->checkState() == State)
    {
      Result |= Bit;
    }
  }
  return Result;
}

void RightsWidget::Private::UpdateChecks(unsigned int Set, unsigned int Unset)
{
  for (const auto & [Check, Bit] : AllChecks())
  {
    QSignalBlocker Blocker(Check);
    Qt::CheckState State =
      (Set & Bit) ? Qt::Checked : ((Unset & Bit) || !AllowUndef) ? Qt::Unchecked : Qt::PartiallyChecked;
    Check->setCheckState(State);
  }
}

void RightsWidget::Private::UpdateOctalEdit()
{
  QSignalBlocker Blocker(OctalEdit);
  // TRightsFrame::UpdateOctalEdit: empty when some bits are undefined
  OctalEdit->setText((Mask(Qt::PartiallyChecked) != 0) ? QString() : Rights::ToOctal(Mask(Qt::Checked)));
}

void RightsWidget::Private::UpdateControls()
{
  DirectoriesXCheck->setVisible(AddXVisible);
  // DirectoriesXEffective: pointless when every X is set already
  DirectoriesXCheck->setEnabled((Mask(Qt::Checked) & 0111) != 0111);
}

RightsWidget::RightsWidget(QWidget * Parent) :
  QWidget(Parent),
  d(new Private)
{
  auto * Grid = new QGridLayout();
  Grid->setContentsMargins(0, 0, 0, 0);
  Grid->setHorizontalSpacing(8);
  Grid->setVerticalSpacing(4);

  static const char * const WhoCaptions[3] = { "&Owner", "&Group", "Ot&hers" };
  static const char * const WhatCaptions[3] = { "R", "W", "X" };
  static const char * const WhatHints[3] = { "Read", "Write", "Execute/Enter" };
  for (int Who = 0; Who < 3; Who++)
  {
    // TSpeedButton: cycles the whole row (CycleRights)
    auto * WhoButton = new QToolButton();
    WhoButton->setText(QString::fromLatin1(WhoCaptions[Who]));
    WhoButton->setAutoRaise(true);
    WhoButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    WhoButton->setObjectName(QStringLiteral("RightsButton%1").arg(Who));
    Grid->addWidget(WhoButton, Who, 0);
    for (int What = 0; What < 3; What++)
    {
      auto * Check = new QCheckBox(QString::fromLatin1(WhatCaptions[What]));
      Check->setToolTip(QString::fromLatin1(WhatHints[What]));
      d->Checks[Who][What] = Check;
      Grid->addWidget(Check, Who, 1 + What);
    }
    connect(WhoButton, &QToolButton::clicked, this, [this, Who]()
    {
      // TRightsFrame::CycleRights: Yes -> No -> Undefined (when allowed) -> Yes;
      // a mixed row becomes Yes
      Qt::CheckState State = d->Checks[Who][0]->checkState();
      bool Same = true;
      for (int What = 1; What < 3; What++)
      {
        Same = Same && (d->Checks[Who][What]->checkState() == State);
      }
      if (!Same)
      {
        State = Qt::Checked;
      }
      else if (State == Qt::Checked)
      {
        State = Qt::Unchecked;
      }
      else if (State == Qt::Unchecked)
      {
        State = d->AllowUndef ? Qt::PartiallyChecked : Qt::Checked;
      }
      else
      {
        State = Qt::Checked;
      }
      for (int What = 0; What < 3; What++)
      {
        QSignalBlocker Blocker(d->Checks[Who][What]);
        d->Checks[Who][What]->setCheckState(State);
      }
      d->UpdateOctalEdit();
      d->UpdateControls();
      emit Changed();
    });
  }
  d->SetUidCheck = new QCheckBox(QStringLiteral("Set UID"));
  d->SetGidCheck = new QCheckBox(QStringLiteral("Set GID"));
  d->StickyBitCheck = new QCheckBox(QStringLiteral("Sticky bit"));
  Grid->addWidget(d->SetUidCheck, 0, 4);
  Grid->addWidget(d->SetGidCheck, 1, 4);
  Grid->addWidget(d->StickyBitCheck, 2, 4);

  auto * OctalLabel = new QLabel(QStringLiteral("O&ctal:"));
  d->OctalEdit = new QLineEdit();
  d->OctalEdit->setMaxLength(4);
  d->OctalEdit->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[0-7]{0,4}")), d->OctalEdit));
  d->OctalEdit->setFixedWidth(70);
  OctalLabel->setBuddy(d->OctalEdit);
  Grid->addWidget(OctalLabel, 3, 0);
  Grid->addWidget(d->OctalEdit, 3, 1, 1, 3, Qt::AlignLeft);
  Grid->setColumnStretch(5, 1);

  d->DirectoriesXCheck = new QCheckBox(QStringLiteral("Add &X to directories"));
  d->DirectoriesXCheck->setVisible(false);

  auto * Layout = new QVBoxLayout(this);
  Layout->setContentsMargins(0, 0, 0, 0);
  Layout->addLayout(Grid);
  Layout->addWidget(d->DirectoriesXCheck);

  for (const auto & [Check, Bit] : d->AllChecks())
  {
    Q_UNUSED(Bit);
    connect(Check, &QCheckBox::checkStateChanged, this, [this]()
    {
      d->UpdateOctalEdit();
      d->UpdateControls();
      emit Changed();
    });
  }
  connect(d->OctalEdit, &QLineEdit::textEdited, this, [this](const QString & Text)
  {
    unsigned int Mode = 0;
    if (Rights::FromOctal(Text, Mode))
    {
      d->UpdateChecks(Mode & 07777, ~Mode & 07777);
      d->UpdateControls();
    }
    emit Changed();
  });
  connect(d->OctalEdit, &QLineEdit::editingFinished, this, [this]()
  {
    // TRightsFrame::OctalEditExit: normalize to 4 digits
    unsigned int Mode = 0;
    if (Rights::FromOctal(d->OctalEdit->text(), Mode))
    {
      QSignalBlocker Blocker(d->OctalEdit);
      d->OctalEdit->setText(Rights::ToOctal(Mode));
    }
  });
  connect(d->DirectoriesXCheck, &QCheckBox::toggled, this, &RightsWidget::Changed);

  SetRights(0644);
  d->setParent(this);
}

// Programmatic change; does not emit Changed()
void RightsWidget::SetRights(unsigned int Mode)
{
  SetStates(Mode & 07777, ~Mode & 07777);
}

void RightsWidget::SetStates(unsigned int Set, unsigned int Unset)
{
  d->UpdateChecks(Set & 07777, Unset & 07777);
  d->UpdateOctalEdit();
  d->UpdateControls();
}

unsigned int RightsWidget::Rights() const
{
  return d->Mask(Qt::Checked);
}

unsigned int RightsWidget::RightsUnset() const
{
  return d->Mask(Qt::Unchecked);
}

bool RightsWidget::IsUndef() const
{
  return d->Mask(Qt::PartiallyChecked) != 0;
}

void RightsWidget::SetAllowUndef(bool Value)
{
  d->AllowUndef = Value;
  for (const auto & [Check, Bit] : d->AllChecks())
  {
    Q_UNUSED(Bit);
    QSignalBlocker Blocker(Check);
    if (!Value && (Check->checkState() == Qt::PartiallyChecked))
    {
      // when disallowing undef state, all undef are turned into unset
      Check->setCheckState(Qt::Unchecked);
    }
    Check->setTristate(Value);
  }
  d->UpdateOctalEdit();
  d->UpdateControls();
}

bool RightsWidget::AllowUndef() const
{
  return d->AllowUndef;
}

void RightsWidget::SetAddXToDirectoriesVisible(bool Visible)
{
  d->AddXVisible = Visible;
  d->UpdateControls();
}

bool RightsWidget::AddXToDirectories() const
{
  return d->AddXVisible && d->DirectoriesXCheck->isEnabled() && d->DirectoriesXCheck->isChecked();
}

void RightsWidget::SetAddXToDirectories(bool Value)
{
  QSignalBlocker Blocker(d->DirectoriesXCheck);
  d->DirectoriesXCheck->setChecked(Value);
}

bool RightsWidget::IsValid() const
{
  if (IsUndef() && d->OctalEdit->text().isEmpty())
  {
    return true;
  }
  unsigned int Mode = 0;
  return Rights::FromOctal(d->OctalEdit->text(), Mode);
}

//---------------------------------------------------------------------------
// PropertiesDialog  (TPropertiesDialog: Common and Checksum tabs)
//---------------------------------------------------------------------------

static QString FormatBytes(qint64 Bytes)
{
  // BaseUtils.FormatBytes, fbShort style
  QLocale Locale;
  if (Bytes < 1024)
  {
    return QStringLiteral("%1 B").arg(Locale.toString(Bytes));
  }
  double Order;
  QString SizeUnit;
  if (Bytes < 1000LL * 1024)
  {
    Order = 1024.0;
    SizeUnit = QStringLiteral("KB");
  }
  else if (Bytes < 1000LL * 1024 * 1024)
  {
    Order = 1024.0 * 1024;
    SizeUnit = QStringLiteral("MB");
  }
  else
  {
    Order = 1024.0 * 1024 * 1024;
    SizeUnit = QStringLiteral("GB");
  }
  double Value = Bytes / Order;
  QString Number;
  if (Value >= 100)
  {
    Number = Locale.toString(static_cast<qint64>(std::floor(Value)));
  }
  else if (Value >= 10)
  {
    Number = Locale.toString(std::floor(Value * 10) / 10, 'f', 1);
  }
  else
  {
    Number = Locale.toString(std::floor(Value * 100) / 100, 'f', 2);
  }
  return QStringLiteral("%1 %2").arg(Number, SizeUnit);
}

static QFrame * MakeBevel()
{
  auto * Bevel = new QFrame();
  Bevel->setFrameShape(QFrame::HLine);
  Bevel->setFrameShadow(QFrame::Sunken);
  return Bevel;
}

// Read-only borderless TEdit, so that the values can be selected and copied
static QLabel * MakeValueLabel(const QString & Text)
{
  auto * Label = new QLabel(Text);
  Label->setTextFormat(Qt::PlainText);
  Label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  Label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  return Label;
}

static QComboBox * MakeTokenCombo()
{
  auto * Combo = new QComboBox();
  Combo->setEditable(true);
  Combo->setInsertPolicy(QComboBox::NoInsert);
  Combo->setMaxVisibleItems(16);
  return Combo;
}

struct PropertiesDialog::Private : public QObject
{
  QList<RemoteFile> Files;
  bool Multiple = false;
  bool Remote = false;
  bool AnyDirectories = false;
  bool AllowCalculateStats = false;
  bool StatsNotCalculated = false;
  bool MultipleChecksum = false;
  bool ChecksumLoaded = false;
  int AllowedChanges = cpMode;
  qint64 FilesSize = 0;
  // original properties
  unsigned int OrigSet = 0;
  unsigned int OrigUnset = 0;
  QString OrigOwner;
  QString OrigGroup;
  QString AlgUsed;

  QTabWidget * PageControl = nullptr;
  QLabel * FileLabel = nullptr;
  QLabel * SizeLabel = nullptr;
  QPushButton * CalculateSizeButton = nullptr;
  QLabel * OwnerLabel = nullptr;
  QLabel * GroupLabel = nullptr;
  QComboBox * OwnerComboBox = nullptr;
  QComboBox * GroupComboBox = nullptr;
  QLabel * OwnerView = nullptr;
  QLabel * GroupView = nullptr;
  QLabel * RightsLabel = nullptr;
  RightsWidget * RightsFrame = nullptr;
  QFrame * RecursiveBevel = nullptr;
  QCheckBox * RecursiveCheck = nullptr;
  QPushButton * OkButton = nullptr;
  // Checksum tab
  QWidget * ChecksumSheet = nullptr;
  QComboBox * ChecksumAlgEdit = nullptr;
  QPushButton * ChecksumButton = nullptr;
  QTreeWidget * ChecksumView = nullptr;
  QGroupBox * ChecksumGroup = nullptr;
  QLineEdit * ChecksumEdit = nullptr;
  QLabel * ChecksumUnknownLabel = nullptr;

  CalculateSizeFunc OnCalculateSize;
  CalculateChecksumFunc OnCalculateChecksum;

  void LoadStats(qint64 Size, const SizeStats & Stats);
  void LoadTokens();
  void UpdateControls();
  void ResetChecksum();
  void CalculateChecksum(PropertiesDialog * Dialog);
  bool ChecksumSupported() const { return static_cast<bool>(OnCalculateChecksum); }
  RemoteProperties GetProperties() const;
};

void PropertiesDialog::Private::LoadStats(qint64 Size, const SizeStats & Stats)
{
  // TPropertiesDialog::LoadStats
  QString SizeStr;
  if (StatsNotCalculated)
  {
    SizeStr = QStringLiteral("Unknown");
  }
  else
  {
    SizeStr = FormatBytes(Size);
    QString SizeUnorderedStr = QStringLiteral("%1 B").arg(QLocale().toString(Size));
    if (SizeStr != SizeUnorderedStr)
    {
      SizeStr = QStringLiteral("%1 (%2)").arg(SizeStr, SizeUnorderedStr);
    }
  }

  QString FilesStr;
  if (((Stats.Files + Stats.Directories) == 0) && !Multiple)
  {
    FilesStr = Files.first().FileName;
  }
  else
  {
    if (Stats.Files > 0)
    {
      FilesStr = (Stats.Files == 1) ? QStringLiteral("%1 file").arg(Stats.Files) : QStringLiteral("%1 files").arg(Stats.Files);
      if (Stats.Directories > 0)
      {
        FilesStr += QStringLiteral(", ");
      }
    }
    if (Stats.Directories > 0)
    {
      FilesStr += (Stats.Directories == 1) ? QStringLiteral("%1 folder").arg(Stats.Directories) :
        QStringLiteral("%1 folders").arg(Stats.Directories);
    }
    if (Stats.SymLinks > 0)
    {
      FilesStr += (Stats.SymLinks == 1) ? QStringLiteral(" (%1 symbolic link)").arg(Stats.SymLinks) :
        QStringLiteral(" (%1 symbolic links)").arg(Stats.SymLinks);
    }
  }
  SizeLabel->setText(SizeStr);
  FileLabel->setText(FilesStr);
}

void PropertiesDialog::Private::LoadTokens()
{
  // TPropertiesDialog::LoadRemoteToken: combo when the change is allowed,
  // read-only view when not (hidden when unknown)
  auto Load = [this](QComboBox * Combo, QLabel * View, QLabel * Label, const QString & Value, int Change)
  {
    bool AllowedChange = (AllowedChanges & Change) != 0;
    Combo->setEditText(Value);
    View->setText(Value);
    Combo->setVisible(AllowedChange);
    View->setVisible(!Value.isEmpty() && !AllowedChange);
    Label->setBuddy(AllowedChange ? static_cast<QWidget *>(Combo) : static_cast<QWidget *>(View));
    Label->setVisible(AllowedChange || !Value.isEmpty());
  };
  Load(OwnerComboBox, OwnerView, OwnerLabel, OrigOwner, cpOwner);
  Load(GroupComboBox, GroupView, GroupLabel, OrigGroup, cpGroup);
  // Recursion is always supported for permissions
  RecursiveCheck->setVisible(AnyDirectories && ((AllowedChanges & (cpMode | cpOwner | cpGroup)) != 0));
  RecursiveBevel->setVisible(!RecursiveCheck->isHidden());
}

RemoteProperties PropertiesDialog::Private::GetProperties() const
{
  // TPropertiesDialog::GetFileProperties + TRemoteProperties::ChangedProperties
  RemoteProperties Result;
  bool Recursive = !RecursiveCheck->isHidden() && RecursiveCheck->isChecked();
  if (AllowedChanges & cpMode)
  {
    unsigned int Set = RightsFrame->RightsSet();
    unsigned int Unset = RightsFrame->RightsUnset();
    bool AddX = RightsFrame->AddXToDirectories();
    if (Recursive || (Set != OrigSet) || (Unset != OrigUnset) || AddX)
    {
      Result.ValidRights = true;
      Result.RightsSet = Set;
      Result.RightsUnset = Unset;
      Result.AddXToDirectories = AddX;
    }
  }
  if (AllowedChanges & cpOwner)
  {
    QString Owner = OwnerComboBox->currentText().trimmed();
    if (!Owner.isEmpty() && (Recursive || (Owner != OrigOwner)))
    {
      Result.Owner = Owner;
    }
  }
  if (AllowedChanges & cpGroup)
  {
    QString Group = GroupComboBox->currentText().trimmed();
    if (!Group.isEmpty() && (Recursive || (Group != OrigGroup)))
    {
      Result.Group = Group;
    }
  }
  Result.Recursive = Recursive;
  return Result;
}

void PropertiesDialog::Private::UpdateControls()
{
  // TPropertiesDialog::UpdateControls
  bool AnyAllowedChanges = (AllowedChanges & (cpMode | cpOwner | cpGroup)) != 0;
  RecursiveCheck->setEnabled(AnyAllowedChanges);
  if (!Multiple)
  {
    // when setting properties for one file only, allow undef state
    // only when "recursive" is on (possible for directory only)
    bool AllowUndef = !RecursiveCheck->isHidden() && RecursiveCheck->isChecked();
    if (RightsFrame->AllowUndef() != AllowUndef)
    {
      QSignalBlocker Blocker(RightsFrame);
      RightsFrame->SetAllowUndef(AllowUndef);
    }
  }
  RemoteProperties Changed = GetProperties();
  bool Allow =
    !(Changed.Owner.isEmpty() && Changed.Group.isEmpty() && !Changed.ValidRights) ||
    (RecursiveCheck->isEnabled() && !RecursiveCheck->isHidden() && RecursiveCheck->isChecked());
  OkButton->setEnabled(Allow && RightsFrame->IsValid());
  OwnerComboBox->setEnabled((AllowedChanges & cpOwner) != 0);
  GroupComboBox->setEnabled((AllowedChanges & cpGroup) != 0);
  RightsFrame->setEnabled((AllowedChanges & cpMode) != 0);
  RightsFrame->setVisible((AllowedChanges & cpMode) != 0);
  RightsLabel->setVisible(!RightsFrame->isHidden());
  CalculateSizeButton->setVisible(AllowCalculateStats && static_cast<bool>(OnCalculateSize));

  ChecksumSheet->setEnabled(ChecksumSupported());
  ChecksumButton->setEnabled(ChecksumSupported() && !ChecksumAlgEdit->currentText().isEmpty());
  ChecksumEdit->setVisible(!ChecksumEdit->text().isEmpty());
  ChecksumUnknownLabel->setVisible(ChecksumEdit->isHidden());
  // the checksum button is the default while the algorithm combo is focused
  bool AlgFocused = ChecksumAlgEdit->hasFocus() || ((ChecksumAlgEdit->lineEdit() != nullptr) && ChecksumAlgEdit->lineEdit()->hasFocus());
  ChecksumButton->setDefault(AlgFocused);
  OkButton->setDefault(!AlgFocused);
}

void PropertiesDialog::Private::ResetChecksum()
{
  ChecksumView->clear();
  ChecksumEdit->clear();
}

void PropertiesDialog::Private::CalculateChecksum(PropertiesDialog * Dialog)
{
  // TPropertiesDialog::CalculateChecksum
  ResetChecksum();
  ChecksumLoaded = true;
  AlgUsed.clear();
  QPointer<PropertiesDialog> Guard(Dialog);
  Dialog->setEnabled(false);
  OnCalculateChecksum(ChecksumAlgEdit->currentText().trimmed(),
    [this, Guard](const QString & FileName, const QString & Alg, const QString & Checksum)
    {
      if (!Guard)
      {
        return;
      }
      // TPropertiesDialog::CalculatedChecksum
      if (MultipleChecksum)
      {
        auto * Item = new QTreeWidgetItem(ChecksumView, {FileName, Checksum});
        Q_UNUSED(Item);
      }
      else
      {
        ChecksumEdit->setText(Checksum);
      }
      AlgUsed = Alg;
    });
  if (!Guard)
  {
    return;
  }
  Dialog->setEnabled(true);
  // If we successfully used the selected checksum, remember it (in normalized form)
  if (!AlgUsed.isEmpty())
  {
    SetConfiguredChecksumAlg(AlgUsed);
  }
  ChecksumView->resizeColumnToContents(0);
  UpdateControls();
}

PropertiesDialog::PropertiesDialog(const QList<RemoteFile> & Files, const QString & Location, bool Remote, QWidget * Parent) :
  QDialog(Parent),
  d(new Private)
{
  d->Files = Files;
  d->Remote = Remote;
  d->Multiple = (Files.size() != 1);
  d->MultipleChecksum = d->Multiple;
  SizeStats Stats;
  for (const RemoteFile & File : Files)
  {
    if (File.IsDirectory)
    {
      Stats.Directories++;
      if (!File.IsSymLink)
      {
        d->AllowCalculateStats = true;
        d->StatsNotCalculated = true;
        d->MultipleChecksum = true;
      }
    }
    else
    {
      Stats.Files++;
      d->FilesSize += File.Size;
    }
    if (File.IsSymLink)
    {
      Stats.SymLinks++;
    }
  }
  d->AnyDirectories = (Stats.Directories > 0);
  const RemoteFile First = Files.isEmpty() ? RemoteFile() : Files.first();
  if (Files.isEmpty())
  {
    d->Files.append(First);
  }

  // Caption: PROPERTIES_FILE_CAPTION / PROPERTIES_FILES_CAPTION
  setWindowTitle(d->Multiple ? QStringLiteral("%1, ... Properties").arg(First.FileName) :
    QStringLiteral("%1 Properties").arg(First.FileName));

  // Owner/group: shown when common to all files; rights: tri-state
  // (all set / all unset / mixed = undefined)
  d->OrigOwner = First.Owner;
  d->OrigGroup = First.Group;
  unsigned int AllSet = 07777, AnySet = 0;
  for (const RemoteFile & File : d->Files)
  {
    if (File.Owner != d->OrigOwner) d->OrigOwner.clear();
    if (File.Group != d->OrigGroup) d->OrigGroup.clear();
    AllSet &= File.Permissions & 07777;
    AnySet |= File.Permissions & 07777;
  }
  d->OrigSet = AllSet;
  d->OrigUnset = ~AnySet & 07777;

  d->PageControl = new QTabWidget();
  auto * CommonSheet = new QWidget();
  d->PageControl->addTab(CommonSheet, QStringLiteral("Common"));

  auto * Grid = new QGridLayout(CommonSheet);
  Grid->setColumnMinimumWidth(0, 80);
  Grid->setColumnStretch(1, 1);
  int Row = 0;

  auto * FileIconImage = new QLabel();
  QStyle::StandardPixmap IconId =
    d->Multiple ? QStyle::SP_FileDialogDetailedView :
    First.IsSymLink ? (First.IsDirectory ? QStyle::SP_DirLinkIcon : QStyle::SP_FileLinkIcon) :
    First.IsDirectory ? QStyle::SP_DirIcon : QStyle::SP_FileIcon;
  FileIconImage->setPixmap(style()->standardIcon(IconId).pixmap(32, 32));
  FileIconImage->setFixedSize(32, 32);
  Grid->addWidget(FileIconImage, Row, 0, Qt::AlignLeft);
  d->FileLabel = MakeValueLabel(QString());
  d->FileLabel->setObjectName(QStringLiteral("FileLabel"));
  Grid->addWidget(d->FileLabel, Row, 1, 1, 2);
  Row++;

  Grid->addWidget(MakeBevel(), Row++, 0, 1, 3);

  Grid->addWidget(new QLabel(QStringLiteral("Location:")), Row, 0);
  Grid->addWidget(MakeValueLabel(Location), Row++, 1, 1, 2);
  Grid->addWidget(new QLabel(QStringLiteral("Size:")), Row, 0);
  d->SizeLabel = MakeValueLabel(QString());
  d->SizeLabel->setObjectName(QStringLiteral("SizeLabel"));
  Grid->addWidget(d->SizeLabel, Row, 1);
  d->CalculateSizeButton = new QPushButton(QStringLiteral("C&alculate"));
  d->CalculateSizeButton->setAutoDefault(false);
  d->CalculateSizeButton->setObjectName(QStringLiteral("CalculateSizeButton"));
  Grid->addWidget(d->CalculateSizeButton, Row++, 2);
  if (!d->Multiple && First.IsSymLink)
  {
    Grid->addWidget(new QLabel(QStringLiteral("Links to:")), Row, 0);
    Grid->addWidget(MakeValueLabel(First.LinkTo), Row++, 1, 1, 2);
  }

  Grid->addWidget(MakeBevel(), Row++, 0, 1, 3);

  d->OwnerLabel = new QLabel(QStringLiteral("Owner:"));
  d->OwnerComboBox = MakeTokenCombo();
  d->OwnerComboBox->setObjectName(QStringLiteral("OwnerComboBox"));
  d->OwnerView = MakeValueLabel(QString());
  auto * OwnerRow = new QHBoxLayout();
  OwnerRow->setContentsMargins(0, 0, 0, 0);
  OwnerRow->addWidget(d->OwnerComboBox, 1);
  OwnerRow->addWidget(d->OwnerView, 1);
  Grid->addWidget(d->OwnerLabel, Row, 0);
  Grid->addLayout(OwnerRow, Row++, 1, 1, 2);
  d->GroupLabel = new QLabel(QStringLiteral("Group:"));
  d->GroupComboBox = MakeTokenCombo();
  d->GroupComboBox->setObjectName(QStringLiteral("GroupComboBox"));
  d->GroupView = MakeValueLabel(QString());
  auto * GroupRow = new QHBoxLayout();
  GroupRow->setContentsMargins(0, 0, 0, 0);
  GroupRow->addWidget(d->GroupComboBox, 1);
  GroupRow->addWidget(d->GroupView, 1);
  Grid->addWidget(d->GroupLabel, Row, 0);
  Grid->addLayout(GroupRow, Row++, 1, 1, 2);

  Grid->addWidget(MakeBevel(), Row++, 0, 1, 3);

  d->RightsLabel = new QLabel(QStringLiteral("Permissions:"));
  d->RightsFrame = new RightsWidget();
  d->RightsFrame->SetAllowUndef(d->Multiple);
  d->RightsFrame->SetStates(d->OrigSet, d->OrigUnset);
  d->RightsFrame->SetAddXToDirectoriesVisible(d->AnyDirectories);
  d->RightsFrame->SetAddXToDirectories(false);
  Grid->addWidget(d->RightsLabel, Row, 0, Qt::AlignTop);
  Grid->addWidget(d->RightsFrame, Row++, 1, 1, 2);

  d->RecursiveBevel = MakeBevel();
  Grid->addWidget(d->RecursiveBevel, Row++, 0, 1, 3);
  d->RecursiveCheck = new QCheckBox(QStringLiteral("Set owner, group and permissions &recursively"), CommonSheet);
  d->RecursiveCheck->setObjectName(QStringLiteral("RecursiveCheck"));
  Grid->addWidget(d->RecursiveCheck, Row++, 0, 1, 3);
  Grid->setRowStretch(Row, 1);

  // Checksum tab
  d->ChecksumSheet = new QWidget();
  d->PageControl->addTab(d->ChecksumSheet, QStringLiteral("Checksum"));
  auto * ChecksumLayout = new QVBoxLayout(d->ChecksumSheet);
  auto * AlgRow = new QHBoxLayout();
  auto * AlgLabel = new QLabel(QStringLiteral("&Algorithm:"));
  d->ChecksumAlgEdit = new QComboBox();
  d->ChecksumAlgEdit->setEditable(true);
  d->ChecksumAlgEdit->setInsertPolicy(QComboBox::NoInsert);
  d->ChecksumAlgEdit->setObjectName(QStringLiteral("ChecksumAlgEdit"));
  AlgLabel->setBuddy(d->ChecksumAlgEdit);
  d->ChecksumButton = new QPushButton(QStringLiteral("&Calculate checksum"));
  d->ChecksumButton->setObjectName(QStringLiteral("ChecksumButton"));
  AlgRow->addWidget(AlgLabel);
  AlgRow->addWidget(d->ChecksumAlgEdit, 1);
  AlgRow->addWidget(d->ChecksumButton);
  ChecksumLayout->addLayout(AlgRow);
  d->ChecksumView = new QTreeWidget();
  d->ChecksumView->setObjectName(QStringLiteral("ChecksumView"));
  d->ChecksumView->setHeaderLabels({QStringLiteral("File"), QStringLiteral("Checksum")});
  d->ChecksumView->setRootIsDecorated(false);
  d->ChecksumView->setSelectionMode(QAbstractItemView::ExtendedSelection);
  d->ChecksumView->setContextMenuPolicy(Qt::CustomContextMenu);
  ChecksumLayout->addWidget(d->ChecksumView, 1);
  d->ChecksumGroup = new QGroupBox(QStringLiteral("Checksum"));
  auto * GroupLayout = new QVBoxLayout(d->ChecksumGroup);
  d->ChecksumEdit = new QLineEdit();
  d->ChecksumEdit->setReadOnly(true);
  d->ChecksumEdit->setObjectName(QStringLiteral("ChecksumEdit"));
  // PROPERTIES_CHECKSUM_UNKNOWN
  d->ChecksumUnknownLabel = new QLabel(QStringLiteral("Unknown"));
  GroupLayout->addWidget(d->ChecksumEdit);
  GroupLayout->addWidget(d->ChecksumUnknownLabel);
  ChecksumLayout->addWidget(d->ChecksumGroup);
  ChecksumLayout->addStretch(d->MultipleChecksum ? 0 : 1);
  d->ChecksumGroup->setVisible(!d->MultipleChecksum);
  d->ChecksumView->setVisible(d->MultipleChecksum);

  auto * Buttons = new QDialogButtonBox();
  d->OkButton = Buttons->addButton(QDialogButtonBox::Ok);
  d->OkButton->setDefault(true);
  Buttons->addButton(QDialogButtonBox::Cancel);
  HelpSystem::AddHelpButton(Buttons, this, QStringLiteral("ui_properties"));
  connect(Buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  auto Update = [this]() { d->UpdateControls(); };
  connect(d->RightsFrame, &RightsWidget::Changed, this, Update);
  connect(d->RecursiveCheck, &QCheckBox::toggled, this, Update);
  connect(d->OwnerComboBox, &QComboBox::currentTextChanged, this, Update);
  connect(d->GroupComboBox, &QComboBox::currentTextChanged, this, Update);
  connect(d->CalculateSizeButton, &QPushButton::clicked, this, [this]()
  {
    // TPropertiesDialog::CalculateSizeButtonClick
    qint64 Size = 0;
    SizeStats Calculated;
    QPointer<PropertiesDialog> Guard(this);
    setEnabled(false);
    bool Ok = d->OnCalculateSize(Size, Calculated);
    if (!Guard)
    {
      return;
    }
    setEnabled(true);
    if (Ok)
    {
      d->StatsNotCalculated = false;
      d->LoadStats(Size, Calculated);
    }
  });
  connect(d->ChecksumButton, &QPushButton::clicked, this, [this]() { d->CalculateChecksum(this); });
  connect(d->ChecksumAlgEdit, &QComboBox::currentTextChanged, this, [this]()
  {
    d->ResetChecksum();
    d->UpdateControls();
  });
  connect(d->PageControl, &QTabWidget::currentChanged, this, [this]()
  {
    // NeedChecksum: calculated automatically when the tab is opened
    if ((d->PageControl->currentWidget() == d->ChecksumSheet) && !d->ChecksumLoaded && d->ChecksumSupported())
    {
      d->CalculateChecksum(this);
    }
  });
  connect(qApp, &QApplication::focusChanged, this, [this]() { d->UpdateControls(); });
  connect(d->ChecksumView, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint & Pos)
  {
    QMenu Menu(this);
    QAction * CopyAction = Menu.addAction(QStringLiteral("&Copy"));
    CopyAction->setEnabled(!d->ChecksumView->selectedItems().isEmpty());
    if (Menu.exec(d->ChecksumView->viewport()->mapToGlobal(Pos)) == CopyAction)
    {
      // TPropertiesDialog::CopyClick
      QList<QTreeWidgetItem *> Items = d->ChecksumView->selectedItems();
      QStringList Lines;
      for (QTreeWidgetItem * Item : Items)
      {
        QString Entry = Item->text(0);
        if (!Item->text(1).isEmpty())
        {
          Entry += QStringLiteral(" = %1").arg(Item->text(1));
        }
        Lines << Entry;
      }
      QApplication::clipboard()->setText((Items.size() == 1) ? Items.first()->text(1) : Lines.join(QLatin1Char('\n')));
    }
  });

  auto * Layout = new QVBoxLayout(this);
  Layout->addWidget(d->PageControl);
  Layout->addWidget(Buttons);

  if (!d->Multiple)
  {
    // Show only file name, if we have only single file/directory
    Stats = SizeStats();
  }
  d->LoadStats(d->FilesSize, Stats);
  d->LoadTokens();
  d->UpdateControls();

  setMinimumWidth(398);
  d->setParent(this);
}

void PropertiesDialog::SetAllowedChanges(int AllowedChanges)
{
  d->AllowedChanges = AllowedChanges;
  d->LoadTokens();
  d->UpdateControls();
}

void PropertiesDialog::SetUsersGroups(const QStringList & Users, const QStringList & Groups)
{
  auto Fill = [](QComboBox * Combo, const QStringList & Items)
  {
    QString Text = Combo->currentText();
    QSignalBlocker Blocker(Combo);
    Combo->clear();
    Combo->addItems(Items);
    Combo->setEditText(Text);
  };
  Fill(d->OwnerComboBox, Users);
  Fill(d->GroupComboBox, Groups);
}

void PropertiesDialog::SetOnCalculateSize(CalculateSizeFunc OnCalculateSize)
{
  d->OnCalculateSize = std::move(OnCalculateSize);
  d->UpdateControls();
}

void PropertiesDialog::SetChecksum(const QStringList & Algs, CalculateChecksumFunc OnCalculateChecksum)
{
  d->OnCalculateChecksum = std::move(OnCalculateChecksum);
  {
    QSignalBlocker Blocker(d->ChecksumAlgEdit);
    d->ChecksumAlgEdit->clear();
    d->ChecksumAlgEdit->addItems(Algs);
    int Index = -1;
    QString Configured = ConfiguredChecksumAlg();
    for (int I = 0; I < Algs.size(); I++)
    {
      if (Algs[I].compare(Configured, Qt::CaseInsensitive) == 0)
      {
        Index = I;
        break;
      }
    }
    if ((Index < 0) && !Algs.isEmpty())
    {
      Index = 0;
    }
    d->ChecksumAlgEdit->setCurrentIndex(Index);
  }
  d->ResetChecksum();
  d->UpdateControls();
}

RemoteProperties PropertiesDialog::Properties() const
{
  return d->GetProperties();
}

unsigned int PropertiesDialog::Rights() const
{
  return d->RightsFrame->Rights();
}

bool PropertiesDialog::RightsChanged() const
{
  return d->GetProperties().ValidRights;
}

bool PropertiesDialog::Recursive() const
{
  return !d->RecursiveCheck->isHidden() && d->RecursiveCheck->isChecked();
}

bool PropertiesDialog::AddXToDirectories() const
{
  return d->RightsFrame->AddXToDirectories();
}

QString PropertiesDialog::ConfiguredChecksumAlg()
{
  IniStorage Storage(&Configuration::Instance().Ini());
  if (Storage.OpenSubKeyPath(QStringLiteral("Configuration\\Interface"), false))
  {
    return Storage.ReadString(QStringLiteral("ChecksumAlg"), QStringLiteral("sha1"));
  }
  return QStringLiteral("sha1");
}

void PropertiesDialog::SetConfiguredChecksumAlg(const QString & Alg)
{
  if (Alg == ConfiguredChecksumAlg())
  {
    return;
  }
  {
    IniStorage Storage(&Configuration::Instance().Ini());
    if (!Storage.OpenSubKeyPath(QStringLiteral("Configuration\\Interface"), true))
    {
      return;
    }
    Storage.WriteString(QStringLiteral("ChecksumAlg"), Alg);
  }
  Configuration::Instance().Save();
}

void PropertiesDialog::showEvent(QShowEvent * Event)
{
  QDialog::showEvent(Event);
  // TPropertiesDialog::Execute: focus the first editable control
  if (!d->OwnerComboBox->isHidden() && d->OwnerComboBox->isEnabled())
  {
    d->OwnerComboBox->setFocus();
  }
  else if (!d->GroupComboBox->isHidden() && d->GroupComboBox->isEnabled())
  {
    d->GroupComboBox->setFocus();
  }
  else if (!d->RightsFrame->isHidden() && d->RightsFrame->isEnabled())
  {
    d->RightsFrame->setFocus();
  }
  d->UpdateControls();
}

void PropertiesDialog::accept()
{
  // TPropertiesDialog::StoreRemoteToken: "name [id]" needs a numeric id
  // (PROPERTIES_INVALID_OWNER / PROPERTIES_INVALID_GROUP)
  auto Valid = [](const QString & AText)
  {
    QString Text = AText.trimmed();
    qsizetype IDStart = Text.lastIndexOf(QLatin1Char('['));
    if ((IDStart >= 0) && Text.endsWith(QLatin1Char(']')))
    {
      bool Ok = false;
      Text.mid(IDStart + 1, Text.size() - IDStart - 2).toUInt(&Ok);
      return Ok;
    }
    return true;
  };
  if ((d->AllowedChanges & cpGroup) && !Valid(d->GroupComboBox->currentText()))
  {
    ShowErrorMessage(this, QStringLiteral("Enter valid group name."));
    d->GroupComboBox->setFocus();
    return;
  }
  if ((d->AllowedChanges & cpOwner) && !Valid(d->OwnerComboBox->currentText()))
  {
    ShowErrorMessage(this, QStringLiteral("Enter valid owner name."));
    d->OwnerComboBox->setFocus();
    return;
  }
  QDialog::accept();
}

//---------------------------------------------------------------------------
// CreateDirectoryDialog  (TCreateDirectoryDialog)
//---------------------------------------------------------------------------

struct CreateDirectoryDialog::Private : public QObject
{
  QLineEdit * DirectoryEdit = nullptr;
  QCheckBox * SetRightsCheck = nullptr;
  RightsWidget * RightsFrame = nullptr;
  QCheckBox * SaveSettingsCheck = nullptr;
  QPushButton * OkButton = nullptr;
  int AllowedChanges = PropertiesDialog::cpMode;

  void UpdateControls();
};

void CreateDirectoryDialog::Private::UpdateControls()
{
  // a path is allowed: the folder is created relative to the current one
  bool Valid = !DirectoryEdit->text().trimmed().isEmpty();
  if (RightsFrame != nullptr)
  {
    SetRightsCheck->setEnabled((AllowedChanges & PropertiesDialog::cpMode) != 0);
    RightsFrame->setEnabled(SetRightsCheck->isEnabled() && SetRightsCheck->isChecked());
    SaveSettingsCheck->setEnabled(AllowedChanges != 0);
    Valid = Valid && (!RightsFrame->isEnabled() || RightsFrame->IsValid());
  }
  OkButton->setEnabled(Valid);
}

CreateDirectoryDialog::CreateDirectoryDialog(bool Remote, QWidget * Parent) :
  QDialog(Parent),
  d(new Private)
{
  setWindowTitle(QStringLiteral("Create folder"));

  auto * EditLabel = new QLabel(QStringLiteral("New &folder name:"));
  d->DirectoryEdit = new QLineEdit(QStringLiteral("New folder"));
  d->DirectoryEdit->setMaxLength(1000);
  d->DirectoryEdit->setObjectName(QStringLiteral("DirectoryEdit"));
  EditLabel->setBuddy(d->DirectoryEdit);

  auto * Layout = new QVBoxLayout(this);
  Layout->addWidget(EditLabel);
  Layout->addWidget(d->DirectoryEdit);

  if (Remote)
  {
    auto * AttributesGroup = new QGroupBox(QStringLiteral("Attributes"));
    auto * GroupLayout = new QVBoxLayout(AttributesGroup);
    d->SetRightsCheck = new QCheckBox(QStringLiteral("Set pe&rmissions"));
    d->SetRightsCheck->setObjectName(QStringLiteral("SetRightsCheck"));
    d->RightsFrame = new RightsWidget();
    d->RightsFrame->SetRights(0755);
    d->RightsFrame->SetAddXToDirectoriesVisible(false);
    auto * RightsRow = new QHBoxLayout();
    RightsRow->setContentsMargins(16, 0, 0, 0);
    RightsRow->addWidget(d->RightsFrame);
    d->SaveSettingsCheck = new QCheckBox(QStringLiteral("Use &same settings next time"));
    d->SaveSettingsCheck->setObjectName(QStringLiteral("SaveSettingsCheck"));
    GroupLayout->addWidget(d->SetRightsCheck);
    GroupLayout->addLayout(RightsRow);
    GroupLayout->addWidget(d->SaveSettingsCheck);
    Layout->addWidget(AttributesGroup);
    connect(d->SetRightsCheck, &QCheckBox::toggled, this, [this]() { d->UpdateControls(); });
    connect(d->RightsFrame, &RightsWidget::Changed, this, [this]() { d->UpdateControls(); });
  }

  auto * Buttons = new QDialogButtonBox();
  d->OkButton = Buttons->addButton(QDialogButtonBox::Ok);
  d->OkButton->setDefault(true);
  Buttons->addButton(QDialogButtonBox::Cancel);
  HelpSystem::AddHelpButton(Buttons, this, QStringLiteral("ui_create_directory"));
  connect(Buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  Layout->addStretch();
  Layout->addWidget(Buttons);

  connect(d->DirectoryEdit, &QLineEdit::textChanged, this, [this]() { d->UpdateControls(); });
  d->UpdateControls();

  setMinimumWidth(337);
  d->DirectoryEdit->setFocus();
  d->DirectoryEdit->selectAll();
  d->setParent(this);
}

QString CreateDirectoryDialog::DirectoryName() const
{
  return d->DirectoryEdit->text().trimmed();
}

void CreateDirectoryDialog::SetDirectoryName(const QString & Name)
{
  d->DirectoryEdit->setText(Name);
  d->DirectoryEdit->selectAll();
}

bool CreateDirectoryDialog::SetRights() const
{
  return (d->SetRightsCheck != nullptr) && d->SetRightsCheck->isEnabled() && d->SetRightsCheck->isChecked();
}

unsigned int CreateDirectoryDialog::Rights() const
{
  return (d->RightsFrame != nullptr) ? d->RightsFrame->Rights() : 0755;
}

void CreateDirectoryDialog::SetAllowedChanges(int AllowedChanges)
{
  d->AllowedChanges = AllowedChanges;
  d->UpdateControls();
}

void CreateDirectoryDialog::SetProperties(const RemoteProperties & Properties)
{
  if (d->RightsFrame == nullptr)
  {
    return;
  }
  d->SetRightsCheck->setChecked(Properties.ValidRights);
  // expect sensible value even if rights are not set valid
  d->RightsFrame->SetStates(Properties.RightsSet, ~Properties.RightsSet & 07777);
  d->UpdateControls();
}

RemoteProperties CreateDirectoryDialog::Properties() const
{
  RemoteProperties Result;
  if (d->RightsFrame != nullptr)
  {
    Result.ValidRights = SetRights();
    Result.RightsSet = d->RightsFrame->Rights();
    Result.RightsUnset = ~Result.RightsSet & 07777;
  }
  return Result;
}

void CreateDirectoryDialog::SetSaveSettings(bool Value)
{
  if (d->SaveSettingsCheck != nullptr)
  {
    d->SaveSettingsCheck->setChecked(Value);
  }
}

bool CreateDirectoryDialog::SaveSettings() const
{
  return (d->SaveSettingsCheck != nullptr) && d->SaveSettingsCheck->isEnabled() && d->SaveSettingsCheck->isChecked();
}

// TRights::GetText ("rwxr-sr-t"; all bits defined); TRights::SetText is
// RightsFromText of core/CopyParam
static QString RightsText(unsigned int Mode)
{
  return Rights::ToText(Mode);
}

RemoteProperties CreateDirectoryDialog::LoadNewDirectoryProperties()
{
  // TGUIConfiguration::Default: Rights = rfDefault | rfExec, nothing valid
  RemoteProperties Result;
  Result.RightsSet = 0755;
  Result.RightsUnset = ~Result.RightsSet & 07777;
  IniStorage Storage(&Configuration::Instance().Ini());
  if (Storage.OpenSubKeyPath(QStringLiteral("Configuration\\Interface\\NewDirectory2"), false))
  {
    // TRemoteProperties::Load: Valid is a binary (hex) Delphi set, vpRights = bit 0
    bool Ok = false;
    uint Valid = Storage.ReadStringRaw(QStringLiteral("Valid"), QString()).left(2).toUInt(&Ok, 16);
    Result.ValidRights = Ok && ((Valid & 0x01) != 0);
    unsigned int Mode = 0;
    if (Result.ValidRights && RightsFromText(Storage.ReadString(QStringLiteral("Rights"), RightsText(0755)), Mode))
    {
      Result.RightsSet = Mode;
      Result.RightsUnset = ~Mode & 07777;
    }
  }
  return Result;
}

void CreateDirectoryDialog::SaveNewDirectoryProperties(const RemoteProperties & Properties)
{
  {
    IniStorage Storage(&Configuration::Instance().Ini());
    if (!Storage.OpenSubKeyPath(QStringLiteral("Configuration\\Interface\\NewDirectory2"), true))
    {
      return;
    }
    Storage.WriteStringRaw(QStringLiteral("Valid"), Properties.ValidRights ? QStringLiteral("01") : QStringLiteral("00"));
    if (Properties.ValidRights)
    {
      Storage.WriteString(QStringLiteral("Rights"), RightsText(Properties.RightsSet));
    }
  }
  Configuration::Instance().Save();
}

PropertiesDialog::~PropertiesDialog()
{
  // Child widgets are destroyed after this; focus moves while they go away
  // (real displays only), and the focusChanged handler must not touch them
  QObject::disconnect(qApp, nullptr, this, nullptr);
}
