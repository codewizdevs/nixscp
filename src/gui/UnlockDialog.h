// Startup prompt for an encrypted configuration (core/ConfigVault): the
// NixSCP password, or the recovery key. Also used for importing an encrypted
// backup made with another password.
#pragma once

#include <QByteArray>
#include <QString>

class QWidget;

namespace UnlockDialog
{
  // Unlocks FileName (the configuration); false when the user quits
  bool UnlockConfiguration(const QString & FileName);
  // Asks for the password or recovery key of an encrypted backup and returns
  // its contents; false when cancelled
  bool OpenEncryptedBackup(QWidget * Parent, const QString & FileName, const QByteArray & Data, QByteArray & Plaintext);
}
