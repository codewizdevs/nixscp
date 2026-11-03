#include "gui/UnlockDialog.h"

#include "core/ConfigVault.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <functional>

namespace
{

// Shared dialog: Try returns an error message, empty on success
bool AskSecret(QWidget * Parent, const QString & Title, const QString & Text, const QString & Hint, const QString & AcceptCaption,
  const QString & RejectCaption, const std::function<QString(const QString & Secret, bool IsRecoveryKey)> & Try)
{
  QDialog Dialog(Parent);
  Dialog.setWindowTitle(Title);
  auto * Layout = new QVBoxLayout(&Dialog);
  auto * Intro = new QLabel(Text, &Dialog);
  Intro->setWordWrap(true);
  Layout->addWidget(Intro);
  auto * Caption = new QLabel(QStringLiteral("NixSCP &password:"), &Dialog);
  auto * Edit = new QLineEdit(&Dialog);
  Edit->setEchoMode(QLineEdit::Password);
  Edit->setMinimumWidth(380);
  Caption->setBuddy(Edit);
  Layout->addWidget(Caption);
  Layout->addWidget(Edit);
  auto * Recovery = new QCheckBox(QStringLiteral("Use my &recovery key instead"), &Dialog);
  Layout->addWidget(Recovery);
  auto * Error = new QLabel(&Dialog);
  Error->setWordWrap(true);
  Error->setStyleSheet(QStringLiteral("color: #c0392b"));
  Error->hide();
  Layout->addWidget(Error);
  if (!Hint.isEmpty())
  {
    auto * HintLabel = new QLabel(Hint, &Dialog);
    HintLabel->setWordWrap(true);
    HintLabel->setEnabled(false);
    Layout->addWidget(HintLabel);
  }
  auto * Buttons = new QDialogButtonBox(&Dialog);
  QPushButton * Accept = Buttons->addButton(AcceptCaption, QDialogButtonBox::AcceptRole);
  Buttons->addButton(RejectCaption, QDialogButtonBox::RejectRole);
  Accept->setDefault(true);
  Layout->addWidget(Buttons);
  QObject::connect(Recovery, &QCheckBox::toggled, &Dialog, [Caption, Edit](bool On)
  {
    Caption->setText(On ? QStringLiteral("&Recovery key (XXXX-XXXX-...):") : QStringLiteral("NixSCP &password:"));
    Edit->setEchoMode(On ? QLineEdit::Normal : QLineEdit::Password);
    Edit->clear();
    Edit->setFocus();
  });
  QObject::connect(Buttons, &QDialogButtonBox::rejected, &Dialog, &QDialog::reject);
  QObject::connect(Buttons, &QDialogButtonBox::accepted, &Dialog, [&]()
  {
    if (Edit->text().isEmpty())
    {
      return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString Message = Try(Edit->text(), Recovery->isChecked());
    QApplication::restoreOverrideCursor();
    if (Message.isEmpty())
    {
      Dialog.accept();
      return;
    }
    Error->setText(Message);
    Error->show();
    Edit->selectAll();
    Edit->setFocus();
  });
  Edit->setFocus();
  return Dialog.exec() == QDialog::Accepted;
}

}

namespace UnlockDialog
{

bool UnlockConfiguration(const QString & FileName)
{
  return AskSecret(nullptr, QStringLiteral("NixSCP"),
    QStringLiteral("<b>Your NixSCP configuration is encrypted.</b><br>Enter your NixSCP password to open it."),
    QStringLiteral("Changed the password on another computer? This computer still uses the previous password until "
      "it syncs again; the recovery key always works. Configuration: %1").arg(QFileInfo(FileName).absoluteFilePath()),
    QStringLiteral("Unlock"), QStringLiteral("Quit"),
    [&FileName](const QString & Secret, bool IsRecoveryKey)
    {
      QString Error;
      return ConfigVault::Unlock(FileName, Secret, IsRecoveryKey, &Error) ? QString() : Error;
    });
}

bool OpenEncryptedBackup(QWidget * Parent, const QString & FileName, const QByteArray & Data, QByteArray & Plaintext)
{
  // a backup made with the current key opens without asking
  if (ConfigVault::Open(Data, Plaintext))
  {
    return true;
  }
  return AskSecret(Parent, QStringLiteral("Import Configuration"),
    QStringLiteral("<b>\"%1\" is an encrypted NixSCP backup.</b><br>Enter the NixSCP password it was made with, "
      "or its recovery key.").arg(QFileInfo(FileName).fileName()),
    QString(), QStringLiteral("Import"), QStringLiteral("Cancel"),
    [&Data, &Plaintext](const QString & Secret, bool IsRecoveryKey)
    {
      QString Error;
      return ConfigVault::OpenWithSecret(Data, Secret, IsRecoveryKey, Plaintext, &Error) ? QString() : Error;
    });
}

}
