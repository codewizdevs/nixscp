// Port of WinSCP source/forms/RemoteTransfer.{cpp,dfm} (TRemoteTransferDialog)
// and TRemoteMoveDialog of source/forms/Custom.cpp
#include "gui/RemoteTransferDialog.h"
#include "gui/HelpSystem.h"

#include "core/Configuration.h"
#include "core/CopyParam.h"
#include "core/FileMasks.h"
#include "gui/GuiUserInterface.h"
#include "gui/HistoryComboBox.h"
#include "gui/Icons.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

static const QString AnyMask = QStringLiteral("*.*");

// UnixExtractFilePath: up to and including the last slash
static QString UnixExtractFilePath(const QString & Path)
{
  qsizetype P = Path.lastIndexOf(QLatin1Char('/'));
  return (P >= 0) ? Path.left(P + 1) : QString();
}

static QString UnixExtractFileNamePart(const QString & Path)
{
  qsizetype P = Path.lastIndexOf(QLatin1Char('/'));
  return (P >= 0) ? Path.mid(P + 1) : Path;
}

static QString UnixIncludeTrailingBackslash(const QString & Path)
{
  return (Path.isEmpty() || Path.endsWith(QLatin1Char('/'))) ? Path : Path + QLatin1Char('/');
}

static bool UnixSamePath(const QString & Path1, const QString & Path2)
{
  return UnixIncludeTrailingBackslash(Path1) == UnixIncludeTrailingBackslash(Path2);
}

static QLabel * MakeImage(const QString & Name)
{
  auto * Image = new QLabel();
  QIcon Icon = Icons::DialogImage(Name);
  if (!Icon.isNull())
  {
    Image->setPixmap(Icon.pixmap(32, 32));
  }
  Image->setFixedSize(32, 32);
  return Image;
}

// Shared DoValidate part: directory -> "dir/*.*", MULTI_FILES_TO_ONE
static bool ValidateTarget(QWidget * Parent, HistoryComboBox * Combo, bool CheckDirectory, bool Multi,
  const DirectoryExistsFunc & OnDirectoryExists, void * Session)
{
  QString Text = Combo->currentText();
  QString Mask = UnixExtractFileNamePart(Text);
  if (CheckDirectory && !IsFileNameMask(Mask) && OnDirectoryExists && OnDirectoryExists(Session, Text))
  {
    Combo->setEditText(UnixIncludeTrailingBackslash(Text) + AnyMask);
    Mask = AnyMask;
  }
  if (!IsFileNameMask(Mask) && Multi)
  {
    QString Message = FormatMultiFilesToOneConfirmation(Combo->currentText());
    if (MessageDialog(Parent, Message, qtConfirmation, qaOK | qaCancel) == qaCancel)
    {
      return false;
    }
  }
  return true;
}

//---------------------------------------------------------------------------
// RemoteTransferDialog  (TRemoteTransferDialog)
//---------------------------------------------------------------------------

struct RemoteTransferDialog::Private : public QObject
{
  QComboBox * SessionCombo = nullptr;
  HistoryComboBox * DirectoryEdit = nullptr;
  QCheckBox * NotDirectCopyCheck = nullptr;
  QPushButton * OkButton = nullptr;
  QList<SessionEntry> Sessions;
  bool Multi = false;
  Terminal::DirectRemoteCopy AllowDirectCopy = Terminal::drcAllow;
  void * CurrentSession = nullptr;
  DirectoryExistsFunc OnDirectoryExists;
  bool TargetConfirmed = false;
  bool DirectCopy = true;
  QString OriginalTarget;

  void * SelectedSession() const
  {
    int Index = SessionCombo->currentIndex();
    return ((Index >= 0) && (Index < Sessions.size())) ? Sessions[Index].Session : nullptr;
  }
  bool IsCurrentSessionSelected() const { return SelectedSession() == CurrentSession; }
  QString Target() const { return UnixExtractFilePath(DirectoryEdit->currentText()); }
  QString FileMask() const { return UnixExtractFileNamePart(DirectoryEdit->currentText()); }
  void UpdateControls()
  {
    NotDirectCopyCheck->setEnabled(IsCurrentSessionSelected() && (AllowDirectCopy != Terminal::drcDisallow));
    OkButton->setEnabled(!DirectoryEdit->currentText().isEmpty());
  }
  void UpdateNotDirectCopyCheck()
  {
    QSignalBlocker Blocker(NotDirectCopyCheck);
    NotDirectCopyCheck->setChecked(IsCurrentSessionSelected() ? !DirectCopy : true);
  }
};

RemoteTransferDialog::RemoteTransferDialog(QWidget * Parent) :
  QDialog(Parent),
  d(new Private)
{
  // REMOTE_COPY_TITLE
  setWindowTitle(QStringLiteral("Duplicate"));

  auto * Group = new QGroupBox();
  auto * Grid = new QGridLayout(Group);
  Grid->addWidget(MakeImage(QStringLiteral("Duplicate L to R")), 0, 0, 4, 1, Qt::AlignTop);
  auto * SessionLabel = new QLabel(QStringLiteral("Target &session:"));
  d->SessionCombo = new QComboBox();
  d->SessionCombo->setObjectName(QStringLiteral("SessionCombo"));
  SessionLabel->setBuddy(d->SessionCombo);
  Grid->addWidget(SessionLabel, 0, 1);
  Grid->addWidget(d->SessionCombo, 1, 1);
  // REMOTE_TRANSFER_PROMPT2
  auto * PathLabel = new QLabel(QStringLiteral("Target remote &path:"));
  d->DirectoryEdit = new HistoryComboBox(QStringLiteral("RemoteTarget"), nullptr);
  d->DirectoryEdit->setObjectName(QStringLiteral("DirectoryEdit"));
  PathLabel->setBuddy(d->DirectoryEdit);
  Grid->addWidget(PathLabel, 2, 1);
  Grid->addWidget(d->DirectoryEdit, 3, 1);
  d->NotDirectCopyCheck = new QCheckBox(QStringLiteral("Duplicate via local &temporary copy"));
  d->NotDirectCopyCheck->setObjectName(QStringLiteral("NotDirectCopyCheck"));
  Grid->addWidget(d->NotDirectCopyCheck, 4, 1);
  Grid->setColumnStretch(1, 1);

  auto * Buttons = new QDialogButtonBox();
  d->OkButton = Buttons->addButton(QDialogButtonBox::Ok);
  d->OkButton->setDefault(true);
  Buttons->addButton(QDialogButtonBox::Cancel);
  HelpSystem::AddHelpButton(Buttons, this, QStringLiteral("ui_duplicate"));
  connect(Buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  auto * Layout = new QVBoxLayout(this);
  Layout->addWidget(Group);
  Layout->addWidget(Buttons);

  connect(d->DirectoryEdit, &QComboBox::currentTextChanged, this, [this]() { d->UpdateControls(); });
  connect(d->SessionCombo, &QComboBox::currentIndexChanged, this, [this](int Index)
  {
    // TRemoteTransferDialog::SessionComboChange
    if ((Index >= 0) && (Index < d->Sessions.size()))
    {
      d->DirectoryEdit->setEditText(UnixIncludeTrailingBackslash(d->Sessions[Index].Directory) + d->FileMask());
    }
    d->UpdateNotDirectCopyCheck();
    d->UpdateControls();
  });
  connect(d->NotDirectCopyCheck, &QCheckBox::toggled, this, [this](bool Checked)
  {
    if (d->IsCurrentSessionSelected())
    {
      d->DirectCopy = !Checked;
    }
  });

  setMinimumWidth(466);
  d->setParent(this);
}

void RemoteTransferDialog::Init(bool Multi, const QList<SessionEntry> & Sessions, Terminal::DirectRemoteCopy AllowDirectCopy,
  void * CurrentSession, DirectoryExistsFunc OnDirectoryExists, bool TargetConfirmed)
{
  d->Multi = Multi;
  d->Sessions = Sessions;
  d->AllowDirectCopy = AllowDirectCopy;
  d->CurrentSession = CurrentSession;
  d->OnDirectoryExists = std::move(OnDirectoryExists);
  d->TargetConfirmed = TargetConfirmed;
  QSignalBlocker Blocker(d->SessionCombo);
  d->SessionCombo->clear();
  for (const SessionEntry & Entry : Sessions)
  {
    d->SessionCombo->addItem(Entry.Name);
  }
}

bool RemoteTransferDialog::Execute(void *& Session, QString & Target, QString & FileMask, bool & DirectCopy)
{
  {
    QSignalBlocker Blocker(d->SessionCombo);
    for (int Index = 0; Index < d->Sessions.size(); Index++)
    {
      if (d->Sessions[Index].Session == Session)
      {
        d->SessionCombo->setCurrentIndex(Index);
        break;
      }
    }
  }
  d->DirectoryEdit->setEditText(UnixIncludeTrailingBackslash(Target) + FileMask);
  d->DirectCopy = DirectCopy;
  d->OriginalTarget = Target;
  d->UpdateNotDirectCopyCheck();
  d->UpdateControls();
  d->DirectoryEdit->setFocus();
  if (exec() != QDialog::Accepted)
  {
    return false;
  }
  Session = d->SelectedSession();
  d->DirectoryEdit->SaveToHistory();
  Target = d->Target();
  FileMask = d->FileMask();
  DirectCopy = !d->NotDirectCopyCheck->isChecked();
  return true;
}

HistoryComboBox * RemoteTransferDialog::DirectoryEdit() const
{
  return d->DirectoryEdit;
}

void RemoteTransferDialog::accept()
{
  // TRemoteTransferDialog::FormCloseQuery
  bool TargetConfirmed = d->TargetConfirmed && UnixSamePath(d->Target(), d->OriginalTarget);
  if (!ValidateTarget(this, d->DirectoryEdit, !TargetConfirmed, d->Multi, d->OnDirectoryExists, d->SelectedSession()))
  {
    return;
  }
  if (d->IsCurrentSessionSelected() &&
      ((d->AllowDirectCopy == Terminal::drcConfirmCommandSession) || (d->AllowDirectCopy == Terminal::drcConfirmCommandSessionDirs)) &&
      !d->NotDirectCopyCheck->isChecked() &&
      Configuration::Instance().ConfirmCommandSession)
  {
    // REMOTE_COPY_COMMAND_SESSION3 with REMOTE_COPY_COMMAND_SESSION_FILES_DIRECTORIES
    QString ObjectName = (d->AllowDirectCopy == Terminal::drcConfirmCommandSession) ?
      QStringLiteral("file(s)") : QStringLiteral("directory(ies)");
    QString Message = QStringLiteral(
      "**Do you want to open a separate shell session to duplicate the %1?**\n \n"
      "Current session does not support direct duplication of remote %2. A separate shell session may be opened "
      "to process the duplication. Alternatively you may duplicate the %3 via local temporary copy.")
      .arg(ObjectName, ObjectName, ObjectName);
    MessageParams Params;
    Params.Params = qpNeverAskAgainCheck;
    unsigned int Answer = MessageDialog(this, Message, qtConfirmation, qaOK | qaCancel, QStringLiteral("ui_duplicate"), &Params);
    if (Answer == qaNeverAskAgain)
    {
      Configuration::Instance().ConfirmCommandSession = false;
      Configuration::Instance().Save();
    }
    else if (Answer != qaOK)
    {
      return;
    }
  }
  QDialog::accept();
}

//---------------------------------------------------------------------------
// RemoteMoveDialog  (TRemoteMoveDialog)
//---------------------------------------------------------------------------

struct RemoteMoveDialog::Private : public QObject
{
  HistoryComboBox * Combo = nullptr;
  QPushButton * OkButton = nullptr;
  bool Multi = false;
  DirectoryExistsFunc OnDirectoryExists;
};

RemoteMoveDialog::RemoteMoveDialog(bool Multi, DirectoryExistsFunc OnDirectoryExists, QWidget * Parent) :
  QDialog(Parent),
  d(new Private)
{
  // REMOTE_MOVE_TITLE
  setWindowTitle(QStringLiteral("Move"));
  d->Multi = Multi;
  d->OnDirectoryExists = std::move(OnDirectoryExists);

  auto * Grid = new QGridLayout();
  Grid->addWidget(MakeImage(QStringLiteral("Move L to R")), 0, 0, 2, 1, Qt::AlignTop);
  auto * Label = new QLabel(QStringLiteral("Target remote &path:"));
  d->Combo = new HistoryComboBox(QStringLiteral("RemoteTarget"), nullptr);
  d->Combo->setObjectName(QStringLiteral("DirectoryEdit"));
  Label->setBuddy(d->Combo);
  Grid->addWidget(Label, 0, 1);
  Grid->addWidget(d->Combo, 1, 1);
  Grid->setColumnStretch(1, 1);

  auto * Buttons = new QDialogButtonBox();
  d->OkButton = Buttons->addButton(QDialogButtonBox::Ok);
  d->OkButton->setDefault(true);
  Buttons->addButton(QDialogButtonBox::Cancel);
  // TRemoteMoveDialog: TCustomDialog(HELP_REMOTE_MOVE)
  HelpSystem::AddHelpButton(Buttons, this, QStringLiteral("task_move_duplicate"));
  connect(Buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(d->Combo, &QComboBox::currentTextChanged, this, [this](const QString & Text)
  {
    d->OkButton->setEnabled(!Text.isEmpty());
  });

  auto * Layout = new QVBoxLayout(this);
  Layout->addLayout(Grid);
  Layout->addStretch();
  Layout->addWidget(Buttons);
  // The same as TRemoteTransferDialog
  setMinimumWidth(466);
  d->setParent(this);
}

bool RemoteMoveDialog::Execute(QString & Target, QString & FileMask)
{
  d->Combo->setEditText(UnixIncludeTrailingBackslash(Target) + FileMask);
  d->OkButton->setEnabled(!d->Combo->currentText().isEmpty());
  d->Combo->setFocus();
  if (exec() != QDialog::Accepted)
  {
    return false;
  }
  Target = UnixExtractFilePath(d->Combo->currentText());
  FileMask = UnixExtractFileNamePart(d->Combo->currentText());
  d->Combo->SaveToHistory();
  return true;
}

HistoryComboBox * RemoteMoveDialog::DirectoryEdit() const
{
  return d->Combo;
}

void RemoteMoveDialog::accept()
{
  // TRemoteMoveDialog::DoValidate
  if (!ValidateTarget(this, d->Combo, true, d->Multi, d->OnDirectoryExists, nullptr))
  {
    return;
  }
  QDialog::accept();
}
