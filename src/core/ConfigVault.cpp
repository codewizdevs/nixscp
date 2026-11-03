#include "core/ConfigVault.h"
#include "core/Configuration.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>

namespace ConfigVault
{

namespace
{
QByteArray GKey;
CloudCrypto::SnapshotHeader GWrap;
bool GEnabled = false;

QString ConfigDirectory()
{
  const Configuration & Config = Configuration::Instance();
  return Config.IsNulStorage() ? QString() : QFileInfo(Config.IniFileName()).absolutePath();
}
}

bool IsSealed(const QByteArray & Data)
{
  if (!Data.startsWith("NXS1"))
  {
    return false;
  }
  CloudCrypto::SnapshotHeader Header;
  return CloudCrypto::ReadHeader(Data, Header) && (Header.Slot == QLatin1String(Slot));
}

bool IsSealedFile(const QString & FileName)
{
  QFile File(FileName);
  if (!File.open(QIODevice::ReadOnly))
  {
    return false;
  }
  // the header is small; the whole file is read only to parse it safely
  return IsSealed(File.readAll());
}

bool Enabled()
{
  return GEnabled && (GKey.size() == CloudCrypto::KeyLength);
}

void SetKey(const QByteArray & Dek, const CloudCrypto::SnapshotHeader & Wrap)
{
  GKey = Dek;
  GWrap = Wrap;
  GWrap.Slot = QString::fromLatin1(Slot);
  // newer than any file written before: an older file never brings old wraps back
  GWrap.SavedAt = QDateTime::currentDateTimeUtc();
  GEnabled = (Dek.size() == CloudCrypto::KeyLength) && !Wrap.DekPassword.isEmpty();
}

QByteArray Key()
{
  return GKey;
}

QString User()
{
  return GWrap.User;
}

CloudCrypto::SnapshotHeader Wrap()
{
  return GWrap;
}

void Disable()
{
  GEnabled = false;
}

void Forget()
{
  GEnabled = false;
  GKey.fill('\0');
  GKey.clear();
  GWrap = CloudCrypto::SnapshotHeader();
}

bool Unlock(const QString & FileName, const QString & Secret, bool IsRecoveryKey, QString * Error)
{
  QFile File(FileName);
  QByteArray Data = File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray();
  CloudCrypto::SnapshotHeader Header;
  if (!IsSealed(Data) || !CloudCrypto::ReadHeader(Data, Header))
  {
    if (Error != nullptr) *Error = QStringLiteral("The file is not an encrypted NixSCP configuration.");
    return false;
  }
  QByteArray Dek;
  if (IsRecoveryKey)
  {
    QByteArray Recovery = CloudCrypto::ParseRecoveryKey(Secret);
    Dek = Recovery.isEmpty() ? QByteArray() : CloudCrypto::UnwrapDekWithRecoveryKey(Header, Recovery);
  }
  else
  {
    Dek = CloudCrypto::UnwrapDekWithPassword(Header, Secret);
  }
  QByteArray Plaintext;
  CloudCrypto::SnapshotHeader Checked;
  if (Dek.isEmpty() || !CloudCrypto::DecodeSnapshot(Data, Dek, Checked, Plaintext))
  {
    if (Error != nullptr)
    {
      *Error = IsRecoveryKey ? QStringLiteral("The recovery key is not correct.") : QStringLiteral("The password is not correct.");
    }
    return false;
  }
  SetKey(Dek, Header);
  return true;
}

bool IsProtectedFile(const QString & FileName)
{
  QString Dir = ConfigDirectory();
  if (Dir.isEmpty())
  {
    return false;
  }
  QFileInfo Info(FileName);
  QString Path = Info.absoluteFilePath();
  if (Path == QFileInfo(Configuration::Instance().IniFileName()).absoluteFilePath())
  {
    return true;
  }
  if (Path == Dir + QStringLiteral("/nixscp-teams.ini"))
  {
    return true;   // team names and sync state
  }
  if (Path == Dir + QStringLiteral("/nixscp-cloud-session.ini"))
  {
    return true;
  }
  if (Info.absolutePath() == Dir + QStringLiteral("/agent-sessions"))
  {
    // transcripts of agent requests
    return true;
  }
  return (Info.absolutePath() == Dir + QStringLiteral("/backups")) && Info.fileName().startsWith(QStringLiteral("nixscp-"));
}

QByteArray Seal(const QByteArray & Plaintext)
{
  CloudCrypto::SnapshotHeader Header = GWrap;
  Header.Slot = QString::fromLatin1(Slot);
  Header.Version = 0;
  Header.SavedAt = QDateTime::currentDateTimeUtc();
  Header.Device.clear();
  return CloudCrypto::EncodeSnapshot(Header, GKey, Plaintext);
}

bool Open(const QByteArray & Data, QByteArray & Plaintext)
{
  if (!IsSealed(Data))
  {
    Plaintext = Data;
    return true;
  }
  if (GKey.size() != CloudCrypto::KeyLength)
  {
    Plaintext.clear();
    return false;
  }
  CloudCrypto::SnapshotHeader Header;
  if (!CloudCrypto::DecodeSnapshot(Data, GKey, Header, Plaintext))
  {
    return false;
  }
  // Same key, newer wraps (the password was changed by another NixSCP running
  // on this computer): adopt them, so the next save does not bring the old back
  if (GEnabled && (Header.User == GWrap.User) && !Header.DekPassword.isEmpty() &&
      ((Header.DekPassword != GWrap.DekPassword) || (Header.DekRecovery != GWrap.DekRecovery)) &&
      (Header.SavedAt > GWrap.SavedAt))
  {
    GWrap.Kdf = Header.Kdf;
    GWrap.DekPassword = Header.DekPassword;
    GWrap.DekRecovery = Header.DekRecovery;
    GWrap.SavedAt = Header.SavedAt;
  }
  return true;
}

bool OpenWithSecret(const QByteArray & Data, const QString & Secret, bool IsRecoveryKey, QByteArray & Plaintext, QString * Error)
{
  CloudCrypto::SnapshotHeader Header;
  if (!IsSealed(Data) || !CloudCrypto::ReadHeader(Data, Header))
  {
    if (Error != nullptr) *Error = QStringLiteral("The file is not an encrypted NixSCP configuration.");
    return false;
  }
  QByteArray Recovery = IsRecoveryKey ? CloudCrypto::ParseRecoveryKey(Secret) : QByteArray();
  QByteArray Dek = IsRecoveryKey ?
    (Recovery.isEmpty() ? QByteArray() : CloudCrypto::UnwrapDekWithRecoveryKey(Header, Recovery)) :
    CloudCrypto::UnwrapDekWithPassword(Header, Secret);
  CloudCrypto::SnapshotHeader Checked;
  if (Dek.isEmpty() || !CloudCrypto::DecodeSnapshot(Data, Dek, Checked, Plaintext))
  {
    if (Error != nullptr)
    {
      *Error = IsRecoveryKey ? QStringLiteral("The recovery key is not correct.") : QStringLiteral("The password is not correct.");
    }
    return false;
  }
  return true;
}

QByteArray SealExport(const QByteArray & Plaintext)
{
  return Seal(Plaintext);
}

QByteArray ForFile(const QString & FileName, const QByteArray & Plaintext)
{
  return (Enabled() && IsProtectedFile(FileName)) ? Seal(Plaintext) : Plaintext;
}

QByteArray ReadFile(const QString & FileName, bool * Locked)
{
  if (Locked != nullptr)
  {
    *Locked = false;
  }
  QFile File(FileName);
  if (!File.open(QIODevice::ReadOnly))
  {
    return QByteArray();
  }
  QByteArray Data = File.readAll();
  QByteArray Plaintext;
  if (!Open(Data, Plaintext))
  {
    if (Locked != nullptr)
    {
      *Locked = true;
    }
    return QByteArray();
  }
  return Plaintext;
}

}
