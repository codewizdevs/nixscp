// Port of WinSCP source/forms/Symlink.{cpp,dfm} (TSymlinkDialog)
#include "gui/LinkDialog.h"
#include "gui/HelpSystem.h"

#include "gui/GuiUserInterface.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

struct LinkDialog::Private : public QObject
{
  QLineEdit * FileNameEdit = nullptr;
  QLineEdit * PointToEdit = nullptr;
  QCheckBox * HardLinkCheck = nullptr;
  QPushButton * OkButton = nullptr;
  bool Edit = false;
  bool AllowHardLink = false;

  void UpdateControls()
  {
    // TSymlinkDialog::UpdateControls
    FileNameEdit->setReadOnly(Edit);
    FileNameEdit->setFocusPolicy(Edit ? Qt::NoFocus : Qt::StrongFocus);
    HardLinkCheck->setEnabled(!Edit && AllowHardLink);
    OkButton->setEnabled(!FileNameEdit->text().isEmpty() && !PointToEdit->text().isEmpty());
  }
};

LinkDialog::LinkDialog(QWidget * Parent) :
  QDialog(Parent),
  d(new Private)
{
  auto * SymlinkGroup = new QGroupBox();
  auto * Grid = new QVBoxLayout(SymlinkGroup);
  auto * FileNameLabel = new QLabel(QStringLiteral("&Link file:"));
  d->FileNameEdit = new QLineEdit();
  d->FileNameEdit->setObjectName(QStringLiteral("FileNameEdit"));
  FileNameLabel->setBuddy(d->FileNameEdit);
  auto * PointToLabel = new QLabel(QStringLiteral("&Point link to:"));
  d->PointToEdit = new QLineEdit();
  d->PointToEdit->setObjectName(QStringLiteral("PointToEdit"));
  PointToLabel->setBuddy(d->PointToEdit);
  d->HardLinkCheck = new QCheckBox(QStringLiteral("&Hard link"));
  d->HardLinkCheck->setObjectName(QStringLiteral("HardLinkCheck"));
  Grid->addWidget(FileNameLabel);
  Grid->addWidget(d->FileNameEdit);
  Grid->addWidget(PointToLabel);
  Grid->addWidget(d->PointToEdit);
  Grid->addWidget(d->HardLinkCheck);

  auto * Buttons = new QDialogButtonBox();
  d->OkButton = Buttons->addButton(QDialogButtonBox::Ok);
  d->OkButton->setDefault(true);
  Buttons->addButton(QDialogButtonBox::Cancel);
  HelpSystem::AddHelpButton(Buttons, this, QStringLiteral("ui_symlink"));
  connect(Buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  auto * Layout = new QVBoxLayout(this);
  Layout->addWidget(SymlinkGroup);
  Layout->addStretch();
  Layout->addWidget(Buttons);

  connect(d->FileNameEdit, &QLineEdit::textChanged, this, [this]() { d->UpdateControls(); });
  connect(d->PointToEdit, &QLineEdit::textChanged, this, [this]() { d->UpdateControls(); });

  setWindowTitle(QStringLiteral("Add link"));
  setMinimumWidth(380);
  d->UpdateControls();
  d->setParent(this);
}

bool LinkDialog::Execute(QString & FileName, QString & PointTo, bool & SymbolicLink, bool Edit, bool AllowHardLink)
{
  SetFileName(FileName);
  SetPointTo(PointTo);
  SetSymbolicLink(SymbolicLink);
  SetEdit(Edit);
  SetAllowHardLink(AllowHardLink);
  if (exec() != QDialog::Accepted)
  {
    return false;
  }
  FileName = this->FileName();
  PointTo = this->PointTo();
  SymbolicLink = this->SymbolicLink();
  return true;
}

QString LinkDialog::FileName() const
{
  return d->FileNameEdit->text();
}

void LinkDialog::SetFileName(const QString & Value)
{
  d->FileNameEdit->setText(Value);
}

QString LinkDialog::PointTo() const
{
  return d->PointToEdit->text();
}

void LinkDialog::SetPointTo(const QString & Value)
{
  d->PointToEdit->setText(Value);
}

bool LinkDialog::SymbolicLink() const
{
  return !d->HardLinkCheck->isChecked();
}

void LinkDialog::SetSymbolicLink(bool Value)
{
  d->HardLinkCheck->setChecked(!Value);
}

void LinkDialog::SetEdit(bool Value)
{
  d->Edit = Value;
  d->UpdateControls();
}

void LinkDialog::SetAllowHardLink(bool Value)
{
  d->AllowHardLink = Value;
  d->UpdateControls();
}

void LinkDialog::showEvent(QShowEvent * Event)
{
  // TSymlinkDialog::FormShow: LINK_EDIT_CAPTION / LINK_ADD_CAPTION
  setWindowTitle(d->Edit ? QStringLiteral("Edit link") : QStringLiteral("Add link"));
  d->UpdateControls();
  QDialog::showEvent(Event);
  if (d->Edit)
  {
    d->PointToEdit->setFocus();
    d->PointToEdit->selectAll();
  }
}
