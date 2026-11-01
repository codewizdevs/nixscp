// Port of WinSCP source/forms/Copy.{cpp,dfm}, plus the dialog part of
// TCustomScpExplorerForm::CopyParamDialog (forms/CustomScpExplorer.cpp)
#include "gui/CopyDialog.h"
#include "gui/HelpSystem.h"
#include "gui/CopyParamsDialog.h"
#include "gui/EditMaskDialog.h"
#include "gui/PreferencesDialog.h"

#include "core/Configuration.h"
#include "core/CopyParamList.h"
#include "core/IniStorage.h"
#include "core/RemoteFile.h"

#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

struct CopyDialog::Private : public QObject
{
  bool ToRemote = true;
  bool Move = false;
  int Options = coDoNotShowAgain;
  int CopyParamAttrs = 0;
  bool DragDrop = false;
  bool SaveSettings = false;
  bool Skipped = false;
  int FileCount = 0;
  QString InitialDirectory;
  QString Preset;
  CopyParam CopyParams;

  QLabel * DirectoryLabel = nullptr;
  QComboBox * DirectoryEdit = nullptr;
  QPushButton * LocalDirectoryBrowseButton = nullptr;
  QGroupBox * CopyParamGroup = nullptr;
  QLabel * CopyParamLabel = nullptr;
  QCheckBox * QueueCheck = nullptr;
  QCheckBox * NeverShowAgainCheck = nullptr;
  QPushButton * TransferSettingsButton = nullptr;
  QToolButton * TransferSettingsDropDown = nullptr;
  QFrame * ShortCutHintPanel = nullptr;
  QPushButton * OkButton = nullptr;
  CopyDialog * Dialog = nullptr;

  int ActualCopyParamAttrs() const;
  void UpdateControls();
  void AdjustControls();
  void EditCopyParams();
  void CopyParamListPopup(const QPoint & GlobalPos, int AdditionalOptions);
  void LocalDirectoryEditExit();
  bool eventFilter(QObject * Watched, QEvent * Event) override;
};

static const QString ShortCutHintKey = QStringLiteral("CopyShortCutHintShown");

static bool CopyShortCutHintShown()
{
  IniStorage Storage(&Configuration::Instance().Ini());
  if (Storage.OpenSubKeyPath(QStringLiteral("Configuration\\Interface"), false))
  {
    return Storage.ReadBool(ShortCutHintKey, false);
  }
  return false;
}

static void SetCopyShortCutHintShown()
{
  IniStorage Storage(&Configuration::Instance().Ini());
  if (Storage.OpenSubKeyPath(QStringLiteral("Configuration\\Interface"), true))
  {
    Storage.WriteBool(ShortCutHintKey, true);
  }
}

static QString IncludeTrailingSlash(const QString & Path)
{
  if (Path.isEmpty() || Path.endsWith(QLatin1Char('/')))
  {
    return Path;
  }
  return Path + QLatin1Char('/');
}

int CopyDialog::Private::ActualCopyParamAttrs() const
{
  return CopyParamAttrs |
    ((QueueCheck->isChecked() && Configuration::Instance().DefaultCopyParam().QueueParallel) ? cpaNoCalculateSize : 0);
}

void CopyDialog::Private::UpdateControls()
{
  // TCopyDialog::UpdateControls
  QString InfoStr = CopyParams.GetInfoStr(QStringLiteral("; "), ActualCopyParamAttrs());
  CopyParamLabel->setText(InfoStr);
  CopyParamLabel->setToolTip(InfoStr);
  QueueCheck->setEnabled((Options & (coDisableQueue | coTemp)) == 0);
  OkButton->setEnabled(((Options & coTemp) != 0) || !DirectoryEdit->currentText().trimmed().isEmpty());
}

void CopyDialog::Private::AdjustControls()
{
  // TCopyDialog::AdjustControls
  bool Temp = ((Options & coTemp) != 0);
  DirectoryEdit->setVisible(!Temp);
  DirectoryLabel->setEnabled(DirectoryEdit->isEnabled());
  if (LocalDirectoryBrowseButton != nullptr)
  {
    LocalDirectoryBrowseButton->setVisible(!ToRemote && !Temp);
  }
  // Command-line transfers have no "Do not show again"
  NeverShowAgainCheck->setVisible((Options & coDoNotShowAgain) != 0);
  ShortCutHintPanel->setVisible(((Options & coShortCutHint) != 0) && !CopyShortCutHintShown());
  UpdateControls();
}

void CopyDialog::Private::EditCopyParams()
{
  // TCopyDialog::CopyParamGroupClick
  if (CopyParamGroup->isEnabled() && CopyParamsDialog::Execute(CopyParams, ActualCopyParamAttrs(), Dialog))
  {
    UpdateControls();
  }
}

void CopyDialog::Private::CopyParamListPopup(const QPoint & GlobalPos, int AdditionalOptions)
{
  // TCopyDialog::CopyParamListPopup + CopyParamClick
  // Save including the preset-unspecific queue properties
  CopyParam Param = Dialog->Param();
  bool PrevSaveSettings = SaveSettings;
  int Result = ::CopyParamListPopup(Dialog, GlobalPos, Param, Preset, AdditionalOptions | cplSaveSettings,
    ActualCopyParamAttrs(), &SaveSettings);
  if (Result > 0)
  {
    CopyParams = Param;
    QueueCheck->setChecked(Param.Queue);
  }
  if (PrevSaveSettings && !SaveSettings)
  {
    NeverShowAgainCheck->setChecked(false);
  }
  UpdateControls();
}

void CopyDialog::Private::LocalDirectoryEditExit()
{
  // TCopyDialog::LocalDirectoryEditExit
  if (!ToRemote && ((Options & coTemp) == 0))
  {
    QString Text = DirectoryEdit->currentText().trimmed();
    if (!Text.isEmpty() && QFileInfo(Text).isDir())
    {
      DirectoryEdit->setEditText(IncludeTrailingSlash(Text) + DefaultCopyFileMask);
    }
  }
}

bool CopyDialog::Private::eventFilter(QObject * Watched, QEvent * Event)
{
  if (Watched == CopyParamGroup)
  {
    // Clicking the transfer settings summary opens the settings, as in WinSCP
    if ((Event->type() == QEvent::MouseButtonRelease) &&
        (static_cast<QMouseEvent *>(Event)->button() == Qt::LeftButton))
    {
      EditCopyParams();
      return true;
    }
    // TCopyDialog::CopyParamGroupContextPopup
    if (Event->type() == QEvent::ContextMenu)
    {
      if (CopyParamGroup->isEnabled())
      {
        CopyParamListPopup(static_cast<QContextMenuEvent *>(Event)->globalPos(), cplCustomizeDefault);
      }
      return true;
    }
  }
  else if ((Watched == ShortCutHintPanel) && (Event->type() == QEvent::MouseButtonRelease))
  {
    // TCopyDialog::ShortCutHintLabelClick (pmCommander)
    PreferencesDialog::Execute(Dialog, QStringLiteral("Commander"));
    return true;
  }
  else if ((Event->type() == QEvent::FocusOut) && Dialog->isVisible() &&
           (DirectoryEdit != nullptr) && (Watched == DirectoryEdit->lineEdit()))
  {
    LocalDirectoryEditExit();
  }
  return QObject::eventFilter(Watched, Event);
}

bool CopyDialog::ConfirmationEnabled(bool DragDrop)
{
  const Configuration & Config = Configuration::Instance();
  return Config.ConfirmTransferring && (!DragDrop || (Config.DDTransferConfirmation != asOff));
}

CopyDialog::CopyDialog(bool ToRemote, bool Move, const QStringList & FileNames, const QString & TargetDirectory,
  const CopyParam & Param, QWidget * Parent) :
  QDialog(Parent),
  d(new Private)
{
  d->ToRemote = ToRemote;
  d->Move = Move;
  d->CopyParams = Param;
  d->CopyParamAttrs = CopyParam::UsableAttrs(ToRemote);
  d->Dialog = this;
  d->FileCount = static_cast<int>(FileNames.size());
  d->InitialDirectory = TargetDirectory;
  // at start assume that copy param is current preset
  d->Preset = CopyParamPresets::Instance().Current();

  // TCopyDialog::AdjustTransferControls
  QString ACaption;
  if (!Move)
  {
    ACaption = ToRemote ? QStringLiteral("Upload") : QStringLiteral("Download");   // COPY_COPY_TO*_CAPTION
  }
  else
  {
    ACaption = ToRemote ? QStringLiteral("Upload and Delete") : QStringLiteral("Download and Delete");
  }
  setWindowTitle(ACaption);

  auto * Image = new QLabel();
  Image->setPixmap(style()->standardIcon(ToRemote ? QStyle::SP_ArrowUp : QStyle::SP_ArrowDown).pixmap(32, 32));
  Image->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
  Image->setFixedWidth(32);

  // COPY_FILE / COPY_FILES / MOVE_FILE / MOVE_FILES
  QString TransferStr = ToRemote ? QStringLiteral("Upload") : QStringLiteral("Download");
  QString DirectionStr = ToRemote ? QStringLiteral("remote directory") : QStringLiteral("local directory");
  QString Label;
  if (FileNames.size() == 1)
  {
    QString FileName = UnixPath::ExtractFileName(FileNames.first());
    if (FileName.isEmpty())
    {
      FileName = FileNames.first();
    }
    Label = (Move ? QStringLiteral("%1 file '%2' to %3 and delete original:") : QStringLiteral("%1 file '%2' to %3:"))
      .arg(TransferStr, FileName, DirectionStr);
  }
  else
  {
    Label = (Move ? QStringLiteral("%1 %2 files to %3 and delete originals:") : QStringLiteral("%1 %2 files to %3:"))
      .arg(TransferStr).arg(FileNames.size()).arg(DirectionStr);
  }
  d->DirectoryLabel = new QLabel(Label);
  d->DirectoryLabel->setTextFormat(Qt::PlainText);

  d->DirectoryEdit = new QComboBox();
  d->DirectoryEdit->setEditable(true);
  d->DirectoryEdit->setInsertPolicy(QComboBox::NoInsert);
  d->DirectoryEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  d->DirectoryEdit->setMinimumContentsLength(40);
  // THistoryComboBox HistoryKey
  EditMaskDialog::LoadHistoryCombo(d->DirectoryEdit, ToRemote ? QStringLiteral("RemoteTarget") : QStringLiteral("LocalTarget"));
  // TCopyDialog::SetParams: Directory + FileMask
  d->DirectoryEdit->setEditText(IncludeTrailingSlash(TargetDirectory) + Param.FileMask);
  d->DirectoryLabel->setBuddy(d->DirectoryEdit);
  d->DirectoryEdit->lineEdit()->installEventFilter(d);

  auto * DirectoryRow = new QHBoxLayout();
  DirectoryRow->addWidget(d->DirectoryEdit, 1);
  if (!ToRemote)
  {
    d->LocalDirectoryBrowseButton = new QPushButton(QStringLiteral("B&rowse..."));
    DirectoryRow->addWidget(d->LocalDirectoryBrowseButton);
    connect(d->LocalDirectoryBrowseButton, &QPushButton::clicked, this, [this]()
    {
      // SELECT_LOCAL_DIRECTORY
      QString Dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Select local directory."), this->TargetDirectory());
      if (!Dir.isEmpty())
      {
        // TCopyDialog::SetDirectory: keeps the file mask
        d->DirectoryEdit->setEditText(IncludeTrailingSlash(Dir) + FileMask());
        d->UpdateControls();
      }
    });
  }

  auto * TopRight = new QVBoxLayout();
  TopRight->setSpacing(3);
  TopRight->addWidget(d->DirectoryLabel);
  TopRight->addLayout(DirectoryRow);

  auto * TopRow = new QHBoxLayout();
  TopRow->addWidget(Image);
  TopRow->addLayout(TopRight, 1);

  // Transfer settings group
  d->CopyParamGroup = new QGroupBox(QStringLiteral("Transfer settings"));
  d->CopyParamGroup->setCursor(Qt::PointingHandCursor);
  d->CopyParamGroup->setContextMenuPolicy(Qt::DefaultContextMenu);
  auto * GroupLayout = new QVBoxLayout(d->CopyParamGroup);
  d->CopyParamLabel = new QLabel();
  d->CopyParamLabel->setWordWrap(true);
  d->CopyParamLabel->setTextFormat(Qt::PlainText);
  d->CopyParamLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
  d->CopyParamLabel->setMinimumHeight(d->CopyParamLabel->fontMetrics().lineSpacing() * 3);
  d->CopyParamLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  GroupLayout->addWidget(d->CopyParamLabel);
  d->CopyParamGroup->installEventFilter(d);

  // COPY_QUEUE / COPY_BACKGROUND
  d->QueueCheck = new QCheckBox(QStringLiteral("Transfer in &background (add to transfer queue)"));
  d->QueueCheck->setChecked(Param.Queue);

  d->NeverShowAgainCheck = new QCheckBox(QStringLiteral("&Do not show this dialog box again"));
  connect(d->NeverShowAgainCheck, &QCheckBox::toggled, this, [this](bool Checked)
  {
    // TCopyDialog::NeverShowAgainCheckClick
    d->SaveSettings = Checked;
    d->UpdateControls();
  });

  // Transfer settings split button
  d->TransferSettingsButton = new QPushButton(QStringLiteral("Transfer settin&gs..."));
  connect(d->TransferSettingsButton, &QPushButton::clicked, this, [this]() { d->EditCopyParams(); });
  d->TransferSettingsDropDown = new QToolButton();
  d->TransferSettingsDropDown->setArrowType(Qt::DownArrow);
  d->TransferSettingsDropDown->setToolTip(QStringLiteral("Transfer settings presets"));
  d->TransferSettingsDropDown->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
  connect(d->TransferSettingsDropDown, &QToolButton::clicked, this, [this]()
  {
    // TCopyDialog::TransferSettingsButtonDropDownClick
    d->CopyParamListPopup(d->TransferSettingsButton->mapToGlobal(QPoint(0, d->TransferSettingsButton->height())),
      cplCustomizeDefault);
  });

  // Commander F5 hint
  d->ShortCutHintPanel = new QFrame();
  d->ShortCutHintPanel->setFrameShape(QFrame::StyledPanel);
  d->ShortCutHintPanel->setAutoFillBackground(true);
  d->ShortCutHintPanel->setBackgroundRole(QPalette::ToolTipBase);
  d->ShortCutHintPanel->setCursor(Qt::PointingHandCursor);
  auto * HintLayout = new QVBoxLayout(d->ShortCutHintPanel);
  auto * ShortCutHintLabel = new QLabel(QStringLiteral(
    "In Commander interface the keyboard shortcut F5 is used to transfer files. "
    "Should you want to use it to refresh a file panel, click here to go to preferences."));
  ShortCutHintLabel->setWordWrap(true);
  ShortCutHintLabel->setForegroundRole(QPalette::ToolTipText);
  ShortCutHintLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
  HintLayout->addWidget(ShortCutHintLabel);
  d->ShortCutHintPanel->installEventFilter(d);

  auto * Buttons = new QDialogButtonBox();
  d->OkButton = Buttons->addButton(QDialogButtonBox::Ok);
  d->OkButton->setDefault(true);
  Buttons->addButton(QDialogButtonBox::Cancel);
  HelpSystem::AddHelpButton(Buttons, this, QStringLiteral("ui_copy"));
  connect(Buttons, &QDialogButtonBox::accepted, this, &CopyDialog::accept);
  connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  auto * Layout = new QVBoxLayout(this);
  Layout->addLayout(TopRow);
  Layout->addWidget(d->CopyParamGroup);
  Layout->addWidget(d->QueueCheck);
  Layout->addWidget(d->NeverShowAgainCheck);
  auto * ButtonRow = new QHBoxLayout();
  auto * SplitButton = new QHBoxLayout();
  SplitButton->setSpacing(0);
  SplitButton->addWidget(d->TransferSettingsButton);
  SplitButton->addWidget(d->TransferSettingsDropDown);
  ButtonRow->addLayout(SplitButton);
  ButtonRow->addWidget(Buttons, 1);
  Layout->addLayout(ButtonRow);
  Layout->addWidget(d->ShortCutHintPanel);
  Layout->setSizeConstraint(QLayout::SetMinAndMaxSize);

  auto Update = [this]() { d->UpdateControls(); };
  connect(d->DirectoryEdit, &QComboBox::editTextChanged, this, Update);
  connect(d->QueueCheck, &QCheckBox::toggled, this, Update);
  d->AdjustControls();

  setMinimumWidth(567);
  d->DirectoryEdit->setFocus();
  d->DirectoryEdit->lineEdit()->selectAll();
  // last child, so it is destroyed after all the widgets referencing it
  d->setParent(this);
}

CopyDialog::~CopyDialog()
{
  // d is a child QObject, deleted by QObject
}

void CopyDialog::SetOptions(int Options)
{
  d->Options = Options;
  d->AdjustControls();
}

int CopyDialog::Options() const
{
  return d->Options;
}

void CopyDialog::SetCopyParamAttrs(int Attrs)
{
  d->CopyParamAttrs = Attrs;
  if ((Attrs & cpaNoTransferMode) != 0)
  {
    // DoCopyDialog: if local and remote EOL types are the same, there is no
    // need for ASCII (or Automatic) mode
    d->CopyParams.Mode = CopyParam::tmBinary;
  }
  d->UpdateControls();
}

void CopyDialog::SetDragDrop(bool DragDrop)
{
  d->DragDrop = DragDrop;
  // GetDoNotShowCopyDialogDefault
  d->NeverShowAgainCheck->setChecked(DragDrop && (Configuration::Instance().DDTransferConfirmation == asAuto));
}

int CopyDialog::OutputOptions() const
{
  return (d->SaveSettings ? cooSaveSettings : 0) | (d->NeverShowAgainCheck->isChecked() ? cooDoNotShowAgain : 0);
}

bool CopyDialog::Skipped() const
{
  return d->Skipped;
}

int CopyDialog::exec()
{
  d->Skipped = false;
  if (((d->Options & coDoNotShowAgain) != 0) && !ConfirmationEnabled(d->DragDrop))
  {
    // TCustomScpExplorerForm::CopyParamDialog: Confirm && ConfirmTransferring
    d->Skipped = true;
    setResult(QDialog::Accepted);
    return QDialog::Accepted;
  }
  int Result = QDialog::exec();
  if (Result == QDialog::Accepted)
  {
    // TCopyDialog::Execute
    Configuration & Config = Configuration::Instance();
    bool Modified = false;
    if ((OutputOptions() & cooSaveSettings) != 0)
    {
      CopyParam Default = Param();
      Default.FileMask = DefaultCopyFileMask;
      Default.ConfirmOverwriting = Config.DefaultCopyParam().ConfirmOverwriting;
      Default.ConfirmResume = Config.DefaultCopyParam().ConfirmResume;
      // the resume settings are not part of the dialog
      Default.ResumeSupport = Config.DefaultCopyParam().ResumeSupport;
      Default.ResumeThreshold = Config.DefaultCopyParam().ResumeThreshold;
      Config.DefaultCopyParam() = Default;
      Modified = true;
    }
    if ((d->Options & coTemp) == 0)
    {
      EditMaskDialog::SaveToHistory(d->ToRemote ? QStringLiteral("RemoteTarget") : QStringLiteral("LocalTarget"),
        d->DirectoryEdit->currentText().trimmed());
      Modified = true;
    }
    if ((d->Options & coShortCutHint) != 0)
    {
      SetCopyShortCutHintShown();
      Modified = true;
    }
    if ((d->Options & coDoNotShowAgain) != 0)
    {
      // TCustomScpExplorerForm::HandleDoNotShowCopyDialogAgain
      if (d->NeverShowAgainCheck->isChecked())
      {
        if (d->DragDrop)
        {
          Config.DDTransferConfirmation = asOff;
        }
        else
        {
          Config.ConfirmTransferring = false;
        }
        Modified = true;
      }
      else if (d->DragDrop && (Config.DDTransferConfirmation == asAuto))
      {
        // User explicitly unchecked "do not show again", so show him the dialog the next time
        Config.DDTransferConfirmation = asOn;
        Modified = true;
      }
    }
    if (Modified)
    {
      Config.Save();
    }
  }
  return Result;
}

void CopyDialog::accept()
{
  // TCopyDialog::FormCloseQuery
  d->LocalDirectoryEditExit();
  if (!d->ToRemote && ((d->Options & coTemp) == 0))
  {
    // CopyDialogValidateLocalDirectory
    QString Directory = TargetDirectory();
    if (!Directory.isEmpty() && !QFileInfo(Directory).isDir())
    {
      if (QMessageBox::question(this, windowTitle(),
            QStringLiteral("Directory '%1' doesn't exist. Create?").arg(Directory),
            QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Ok) != QMessageBox::Ok)
      {
        d->DirectoryEdit->lineEdit()->selectAll();
        d->DirectoryEdit->setFocus();
        return;
      }
      if (!QDir().mkpath(Directory))
      {
        QMessageBox::critical(this, windowTitle(), QStringLiteral("Can't create folder '%1'.").arg(Directory));
        d->DirectoryEdit->lineEdit()->selectAll();
        d->DirectoryEdit->setFocus();
        return;
      }
    }
  }
  if ((d->Options & coTemp) == 0)
  {
    // CopyDialogValidateFileMask
    if (!IsFileNameMask(FileMask()) && (d->FileCount > 1))
    {
      if (QMessageBox::question(this, windowTitle(),
            FormatMultiFilesToOneConfirmation(d->DirectoryEdit->currentText().trimmed()),
            QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Ok)
      {
        d->DirectoryEdit->lineEdit()->selectAll();
        d->DirectoryEdit->setFocus();
        return;
      }
    }
  }
  QDialog::accept();
}

QString CopyDialog::FileMask() const
{
  // TCopyDialog::GetFileMask: ExtractFileName of the target
  if ((d->Options & coTemp) != 0)
  {
    return d->CopyParams.FileMask;
  }
  QString Text = d->DirectoryEdit->currentText().trimmed();
  return Text.mid(Text.lastIndexOf(QLatin1Char('/')) + 1);
}

QString CopyDialog::TargetDirectory() const
{
  // TCopyDialog::GetDirectory: the path part of the target
  if ((d->Options & coTemp) != 0)
  {
    return d->InitialDirectory;
  }
  QString Text = d->DirectoryEdit->currentText().trimmed();
  int P = Text.lastIndexOf(QLatin1Char('/'));
  if (P < 0)
  {
    // only a name (or mask) was typed: it goes to the initial directory
    return d->InitialDirectory;
  }
  Text = Text.left(P + 1);
  while ((Text.size() > 1) && Text.endsWith(QLatin1Char('/')))
  {
    Text.chop(1);
  }
  return Text;
}

CopyParam CopyDialog::Param() const
{
  // TCopyDialog::GetParams
  CopyParam Result = d->CopyParams;
  Result.FileMask = FileMask();
  Result.Queue = Queue();
  return Result;
}

bool CopyDialog::Queue() const
{
  return d->QueueCheck->isEnabled() && d->QueueCheck->isChecked();
}
