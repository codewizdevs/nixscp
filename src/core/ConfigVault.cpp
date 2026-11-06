#include "core/ConfigVault.h"
#include "core/Platform.h"
#include "core/Configuration.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QSaveFile>
#include <QStringList>

namespace ConfigVault
{

namespace
{
SecureBytes GKey;
CloudCrypto::SnapshotHeader GWrap;
bool GEnabled = false;
QStringList GSealedFolders;
// Open() also runs on connection threads (an encrypted SSH key): against the GUI thread's SetKey / Forget
QMutex GKeyLock;

QString ConfigDirectory()
{
  const Configuration & Config = Configuration::Instance();
  return Config.IsNulStorage() ? QString() : QFileInfo(Config.IniFileName()).absolutePath();
}

QString StateFileName(const QString & ConfigFile)
{
  return QFileInfo(ConfigFile).absolutePath() + QStringLiteral("/nixscp-cloud.ini");
}

QString BridgeFileName(const QString & ConfigFile)
{
  return QFileInfo(ConfigFile).absolutePath() + QStringLiteral("/nixscp-cloud-bridge.ini");
}

QByteArray Unwrap(const CloudCrypto::SnapshotHeader & Header, const QString & Secret, bool IsRecoveryKey)
{
  if (!IsRecoveryKey)
  {
    return CloudCrypto::UnwrapDekWithPassword(Header, Secret);
  }
  QByteArray Recovery = CloudCrypto::ParseRecoveryKey(Secret);
  QByteArray Dek = Recovery.isEmpty() ? QByteArray() : CloudCrypto::UnwrapDekWithRecoveryKey(Header, Recovery);
  Security::WipeBytes(Recovery);
  return Dek;
}

void SetWrongSecret(QString * Error, bool IsRecoveryKey)
{
  if (Error != nullptr)
  {
    *Error = IsRecoveryKey ? QStringLiteral("The recovery key is not correct.") : QStringLiteral("The password is not correct.");
  }
}

QByteArray SealAs(const QByteArray & Plaintext, const QString & Role)
{
  QMutexLocker Lock(&GKeyLock);
  CloudCrypto::SnapshotHeader Header = GWrap;
  Header.Slot = QString::fromLatin1(Slot);
  Header.Role = Role;
  Header.ConfigName = (Role == QLatin1String(RoleState)) ? QFileInfo(Configuration::Instance().IniFileName()).fileName() : QString();
  Header.Version = 0;
  Header.SavedAt = QDateTime::currentDateTimeUtc();
  Header.Device.clear();
  return CloudCrypto::EncodeSnapshot(Header, GKey.View(), Plaintext);
}

// Role empty: any role
bool OpenSealed(const QByteArray & Data, const QString & Role, QByteArray & Plaintext)
{
  QMutexLocker Lock(&GKeyLock);
  Plaintext.clear();
  if (GKey.Size() != CloudCrypto::KeyLength)
  {
    return false;
  }
  CloudCrypto::SnapshotHeader Header;
  if (!CloudCrypto::DecodeSnapshot(Data, GKey.View(), Header, Plaintext) || (Header.Slot != QLatin1String(Slot)) ||
      (!Role.isEmpty() && (Header.Role != Role)))
  {
    Plaintext.clear();
    return false;
  }
  // Same key, newer wraps (the password was changed by another NixSCP running
  // on this computer): adopt them, so the next save does not bring the old back
  bool Newer = (Header.WrapEpoch > GWrap.WrapEpoch) || ((Header.WrapEpoch == GWrap.WrapEpoch) && (Header.SavedAt > GWrap.SavedAt));
  if (GEnabled && (Header.User == GWrap.User) && !Header.DekPassword.isEmpty() && Newer &&
      ((Header.DekPassword != GWrap.DekPassword) || (Header.DekRecovery != GWrap.DekRecovery)))
  {
    GWrap.Kdf = Header.Kdf;
    GWrap.DekPassword = Header.DekPassword;
    GWrap.DekRecovery = Header.DekRecovery;
    GWrap.WrapEpoch = Header.WrapEpoch;
    GWrap.SavedAt = Header.SavedAt;
  }
  return true;
}

// Written atomically, owner only; false when it did not reach the disk
bool WriteSealed(const QString & FileName, const QByteArray & Data)
{
  QSaveFile Out(FileName);
  if (!Out.open(QIODevice::WriteOnly))
  {
    return false;
  }
  Out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  Platform::MakePrivate(Out);
  return (Out.write(Data) == Data.size()) && Out.commit();
}

// False only when the file opened with OldKey and could not be written
bool ResealFile(const QString & FileName, const QByteArray & OldKey)
{
  if (!IsSealedFile(FileName))
  {
    return true;
  }
  QFile File(FileName);
  QByteArray Data = File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray();
  File.close();
  CloudCrypto::SnapshotHeader Header;
  QByteArray Plaintext;
  if (!CloudCrypto::DecodeSnapshot(Data, OldKey, Header, Plaintext) || (Header.Slot != QLatin1String(Slot)))
  {
    return true;
  }
  bool Ok = WriteSealed(FileName, SealAs(Plaintext, Header.Role));
  Security::WipeBytes(Plaintext);
  return Ok;
}

// A crash in the middle of ReplaceKey. Only the new key opens the bridge, and the
// configuration that asked for it is then the first thing that moved. Left
// alone otherwise: the key change has not reached this computer, and starts over.
void FinishReplaceKey(const QString & ConfigFile)
{
  QString Name = BridgeFileName(ConfigFile);
  QFile File(Name);
  if (!File.open(QIODevice::ReadOnly))
  {
    return;
  }
  QByteArray Data = File.readAll();
  File.close();
  QByteArray OldKey;
  if (OpenSealed(Data, QString::fromLatin1(RoleBridge), OldKey) && (OldKey.size() == CloudCrypto::KeyLength) && ResealFiles(OldKey))
  {
    QFile::remove(Name);
  }
  Security::WipeBytes(OldKey);
}

QString RefusedProblem(const QString & ConfigFile)
{
  return QStringLiteral("The configuration \"%1\" should be encrypted, but it is missing or not encrypted, "
    "so someone or something may have replaced it. NixSCP will not use it.").arg(ConfigFile);
}

QString StartOverText(const QString & ConfigFile)
{
  return QStringLiteral("To start over without it, delete:\n\n%1\n\nThis computer is then signed out of NixSCP Pro. An "
    "encrypted backup made by this version opens with Import Configuration; a backup made by NixSCP 1.2.3 opens only "
    "in 1.2.3.").arg(StartOverFiles(ConfigFile).join(QLatin1Char('\n')));
}

QString OldFormatProblem(const QString & ConfigFile)
{
  return QStringLiteral("The configuration \"%1\" was encrypted by NixSCP 1.2.3 or older. The file is intact, but this version of "
    "NixSCP cannot read that format, and it cannot be converted.").arg(ConfigFile);
}

QString DamagedProblem(const QString & ConfigFile)
{
  return QStringLiteral("The encrypted configuration \"%1\" is damaged, or was made by another version of NixSCP, so it "
    "cannot be opened.").arg(ConfigFile);
}
}

void (*KeyChangeStep)(const char * Step) = nullptr;

bool IsSealed(const QByteArray & Data)
{
  if (!LooksSealed(Data))
  {
    return false;
  }
  CloudCrypto::SnapshotHeader Header;
  return CloudCrypto::ReadHeader(Data, Header) && (Header.Slot == QLatin1String(Slot));
}

bool LooksSealed(const QByteArray & Data)
{
  return Data.startsWith("NXS1");
}

bool IsSealedFile(const QString & FileName)
{
  QFile File(FileName);
  return File.open(QIODevice::ReadOnly) && LooksSealed(File.read(4));
}

bool RefusesConfig(const QString & ConfigFile)
{
  if (IsSealedFile(ConfigFile))
  {
    return false;
  }
  QFile File(StateFileName(ConfigFile));
  QByteArray Data = File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray();
  if (!LooksSealed(Data))
  {
    return false;
  }
  // Read without the key: whoever could lie here can delete the state as well.
  // A state that does not tell goes with the file of that name in the folder.
  CloudCrypto::SnapshotHeader Header;
  QString Owner = CloudCrypto::ReadHeader(Data, Header) ? Header.ConfigName : QString();
  return QFileInfo(ConfigFile).fileName() == (Owner.isEmpty() ? QStringLiteral("nixscp.ini") : Owner);
}

QString RefusedText(const QString & ConfigFile)
{
  return RefusedProblem(ConfigFile) + QStringLiteral("\n\nPut your encrypted configuration file back. ") + StartOverText(ConfigFile);
}

QString ConfigProblem(const QString & ConfigFile)
{
  if (RefusesConfig(ConfigFile))
  {
    return RefusedProblem(ConfigFile);
  }
  QFile File(ConfigFile);
  QByteArray Data = File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray();
  if (!LooksSealed(Data) || IsSealed(Data))
  {
    return QString();
  }
  return CloudCrypto::IsOldFormat(Data) ? OldFormatProblem(ConfigFile) : DamagedProblem(ConfigFile);
}

QString ConfigProblemText(const QString & ConfigFile)
{
  QString Problem = ConfigProblem(ConfigFile);
  if (Problem.isEmpty())
  {
    return Problem;
  }
  if (RefusesConfig(ConfigFile))
  {
    return RefusedText(ConfigFile);
  }
  QFile File(ConfigFile);
  bool OldFormat = File.open(QIODevice::ReadOnly) && CloudCrypto::IsOldFormat(File.readAll());
  return Problem + (OldFormat ? QStringLiteral("\n\nNixSCP 1.2.3 still opens it, and can export your sites for this version. ")
                              : QStringLiteral("\n\nPut a good copy of the file back. ")) + StartOverText(ConfigFile);
}

QStringList StartOverFiles(const QString & ConfigFile)
{
  QStringList Result;
  QFileInfo Info(ConfigFile);
  const QStringList Names{Info.fileName(), QStringLiteral("nixscp-cloud.ini"), QStringLiteral("nixscp-cloud-session.ini"),
    QStringLiteral("nixscp-cloud-keys.ini"), QStringLiteral("nixscp-cloud-bridge.ini"), QStringLiteral("nixscp-teams.ini")};
  for (const QString & Name : Names)
  {
    QString Path = Info.absolutePath() + QLatin1Char('/') + Name;
    if (QFile::exists(Path))
    {
      Result.append(Path);
    }
  }
  return Result;
}

bool Enabled()
{
  QMutexLocker Lock(&GKeyLock);
  return GEnabled && (GKey.Size() == CloudCrypto::KeyLength);
}

void SetKey(const QByteArray & Dek, const CloudCrypto::SnapshotHeader & Wrap)
{
  QMutexLocker Lock(&GKeyLock);
  GKey.Assign(Dek);
  GWrap = Wrap;
  GWrap.Slot = QString::fromLatin1(Slot);
  // newer than any file written before: an older file never brings old wraps back
  GWrap.SavedAt = QDateTime::currentDateTimeUtc();
  GEnabled = (Dek.size() == CloudCrypto::KeyLength) && !Wrap.DekPassword.isEmpty();
  if (GEnabled)
  {
    Security::ProtectProcessMemory();
  }
}

QByteArray Key()
{
  QMutexLocker Lock(&GKeyLock);
  return GKey.Copy();
}

const SecureBytes & SecureKey()
{
  return GKey;
}

QString User()
{
  QMutexLocker Lock(&GKeyLock);
  return GWrap.User;
}

CloudCrypto::SnapshotHeader Wrap()
{
  QMutexLocker Lock(&GKeyLock);
  return GWrap;
}

void Disable()
{
  QMutexLocker Lock(&GKeyLock);
  GEnabled = false;
}

void Forget()
{
  QMutexLocker Lock(&GKeyLock);
  GEnabled = false;
  GKey.Clear();
  GWrap = CloudCrypto::SnapshotHeader();
}

bool Unlock(const QString & FileName, const QString & Secret, bool IsRecoveryKey, QString * Error)
{
  QFile File(FileName);
  QByteArray Data = File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray();
  CloudCrypto::SnapshotHeader Header;
  if (!IsSealed(Data) || !CloudCrypto::ReadHeader(Data, Header))
  {
    if (Error != nullptr)
    {
      *Error = LooksSealed(Data) ? ConfigProblemText(FileName) : QStringLiteral("The file is not an encrypted NixSCP configuration.");
    }
    return false;
  }
  QByteArray Dek = Unwrap(Header, Secret, IsRecoveryKey);
  QByteArray Plaintext;
  CloudCrypto::SnapshotHeader Checked;
  bool Opened = !Dek.isEmpty() && CloudCrypto::DecodeSnapshot(Data, Dek, Checked, Plaintext);
  Security::WipeBytes(Plaintext);
  if (!Opened)
  {
    // The wrap did open: the secret is right, and the file behind it is not
    bool Damaged = !Dek.isEmpty();
    Security::WipeBytes(Dek);
    SetWrongSecret(Error, IsRecoveryKey);
    if (Damaged && (Error != nullptr))
    {
      *Error = DamagedProblem(FileName) + QStringLiteral("\n\nPut a good copy of the file back. ") + StartOverText(FileName);
    }
    return false;
  }
  // Authenticated now: a backup or a companion file put in the configuration's place
  if (Checked.Role != QLatin1String(RoleConfig))
  {
    Security::WipeBytes(Dek);
    if (Error != nullptr)
    {
      *Error = QStringLiteral("This encrypted file is not a NixSCP configuration (it is of the type \"%1\"). "
        "A backup is restored with Tools > Import Configuration.").arg(Checked.Role);
    }
    return false;
  }
  SetKey(Dek, Checked);
  Security::WipeBytes(Dek);
  FinishReplaceKey(FileName);
  return true;
}

bool CheckSecret(const QString & Secret, bool IsRecoveryKey, QString * Error)
{
  // Argon2id takes a while: the lock is not held for it, connection threads
  // would stand still
  QByteArray Dek = Unwrap(Wrap(), Secret, IsRecoveryKey);
  bool Ok = false;
  {
    QMutexLocker Lock(&GKeyLock);
    Ok = !Dek.isEmpty() && GKey.Equals(Dek);
  }
  Security::WipeBytes(Dek);
  if (!Ok)
  {
    SetWrongSecret(Error, IsRecoveryKey);
  }
  return Ok;
}

QString RoleOf(const QString & FileName)
{
  QString Dir = ConfigDirectory();
  if (Dir.isEmpty())
  {
    return QString();
  }
  QFileInfo Info(FileName);
  QString Path = Info.absoluteFilePath();
  if (Path == QFileInfo(Configuration::Instance().IniFileName()).absoluteFilePath())
  {
    return QString::fromLatin1(RoleConfig);
  }
  if (Path == Dir + QStringLiteral("/nixscp-teams.ini"))
  {
    return QString::fromLatin1(RoleTeams);   // team names and sync state
  }
  if (Path == Dir + QStringLiteral("/nixscp-cloud.ini"))
  {
    return QString::fromLatin1(RoleState);
  }
  if (Path == Dir + QStringLiteral("/nixscp-cloud-session.ini"))
  {
    return QString::fromLatin1(RoleSession);
  }
  if (Path == Dir + QStringLiteral("/nixscp-cloud-keys.ini"))
  {
    return QString::fromLatin1(RoleKeys);
  }
  if (Path == Dir + QStringLiteral("/nixscp-cloud-bridge.ini"))
  {
    return QString::fromLatin1(RoleBridge);
  }
  if (Info.absolutePath() == Dir + QStringLiteral("/agent-sessions"))
  {
    // transcripts of agent requests
    return QString::fromLatin1(RoleAgent);
  }
  if ((Info.absolutePath() == Dir + QStringLiteral("/backups")) && Info.fileName().startsWith(QStringLiteral("nixscp-")))
  {
    return QString::fromLatin1(RoleBackup);
  }
  return QString();
}

bool IsProtectedFile(const QString & FileName)
{
  return !RoleOf(FileName).isEmpty();
}

QByteArray Seal(const QByteArray & Plaintext)
{
  return SealAs(Plaintext, QString::fromLatin1(RoleFile));
}

bool Open(const QByteArray & Data, QByteArray & Plaintext)
{
  if (!LooksSealed(Data))
  {
    Plaintext = Data;
    return true;
  }
  return OpenSealed(Data, QString(), Plaintext);
}

bool OpenFile(const QString & FileName, const QByteArray & Data, QByteArray & Plaintext)
{
  if (LooksSealed(Data))
  {
    return OpenSealed(Data, RoleOf(FileName), Plaintext);
  }
  // Without a sealed state nothing is asked of the configuration: most plain
  // files are read long before there is one, or by users without NixSCP Pro
  if (RefusesConfig(FileName) && (RoleOf(FileName) == QLatin1String(RoleConfig)))
  {
    Plaintext.clear();
    return false;
  }
  Plaintext = Data;
  return true;
}

bool OpenWithSecret(const QByteArray & Data, const QString & Secret, bool IsRecoveryKey, QByteArray & Plaintext, QString * Error)
{
  CloudCrypto::SnapshotHeader Header;
  if (!IsSealed(Data) || !CloudCrypto::ReadHeader(Data, Header))
  {
    if (Error != nullptr)
    {
      *Error = CloudCrypto::IsOldFormat(Data) ? QStringLiteral("This file was encrypted by NixSCP 1.2.3 or older, and this version cannot read that format.")
                                              : QStringLiteral("The file is not an encrypted NixSCP configuration.");
    }
    return false;
  }
  QByteArray Dek = Unwrap(Header, Secret, IsRecoveryKey);
  CloudCrypto::SnapshotHeader Checked;
  bool Opened = !Dek.isEmpty() && CloudCrypto::DecodeSnapshot(Data, Dek, Checked, Plaintext);
  Security::WipeBytes(Dek);
  if (!Opened)
  {
    SetWrongSecret(Error, IsRecoveryKey);
  }
  return Opened;
}

QByteArray SealExport(const QByteArray & Plaintext)
{
  return SealAs(Plaintext, QString::fromLatin1(RoleExport));
}

QByteArray ForFile(const QString & FileName, const QByteArray & Plaintext)
{
  if (!Enabled())
  {
    return Plaintext;
  }
  QString Role = RoleOf(FileName);
  return !Role.isEmpty() ? SealAs(Plaintext, Role) : Plaintext;
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
    // the configuration was removed while the state says it is encrypted
    if ((Locked != nullptr) && RefusesConfig(FileName) && (RoleOf(FileName) == QLatin1String(RoleConfig)))
    {
      *Locked = true;
    }
    return QByteArray();
  }
  QByteArray Data = File.readAll();
  QByteArray Plaintext;
  if (!OpenFile(FileName, Data, Plaintext))
  {
    if (Locked != nullptr)
    {
      *Locked = true;
    }
    return QByteArray();
  }
  return Plaintext;
}

bool ResealFiles(const QByteArray & OldKey)
{
  QStringList Folders = GSealedFolders;
  QString Dir = ConfigDirectory();
  QString ConfigFile;
  QString StateFile;
  if (!Dir.isEmpty())
  {
    Folders << Dir << Dir + QStringLiteral("/backups") << Dir + QStringLiteral("/agent-sessions");
    ConfigFile = QFileInfo(Configuration::Instance().IniFileName()).absoluteFilePath();
    StateFile = StateFileName(ConfigFile);
    if (!ResealFile(ConfigFile, OldKey))
    {
      return false;
    }
    StepReached("config-resealed");
  }
  for (const QString & Folder : Folders)
  {
    const QFileInfoList Files = QDir(Folder).entryInfoList(QDir::Files);
    for (const QFileInfo & Info : Files)
    {
      QString Path = Info.absoluteFilePath();
      if ((Path != ConfigFile) && (Path != StateFile))
      {
        ResealFile(Info.filePath(), OldKey);
      }
    }
  }
  StepReached("files-resealed");
  if (!StateFile.isEmpty())
  {
    ResealFile(StateFile, OldKey);
  }
  return true;
}

bool ReplaceKey(const QByteArray & Dek, const CloudCrypto::SnapshotHeader & Wrap)
{
  QByteArray OldKey = Key();
  CloudCrypto::SnapshotHeader OldWrap = ConfigVault::Wrap();
  SetKey(Dek, Wrap);
  bool Ok = true;
  QString Dir = ConfigDirectory();
  if (!OldKey.isEmpty() && (OldKey != Dek))
  {
    QString Bridge = BridgeFileName(Configuration::Instance().IniFileName());
    Ok = Dir.isEmpty() || WriteSealed(Bridge, SealAs(OldKey, QString::fromLatin1(RoleBridge)));
    StepReached("bridge-written");
    Ok = Ok && ResealFiles(OldKey);
    if (!Ok)
    {
      SetKey(OldKey, OldWrap);
      QFile::remove(Bridge);
    }
  }
  Security::WipeBytes(OldKey);
  return Ok;
}

void EndReplaceKey()
{
  if (!ConfigDirectory().isEmpty())
  {
    QFile::remove(BridgeFileName(Configuration::Instance().IniFileName()));
  }
}

void AddSealedFolder(const QString & Folder)
{
  if (!GSealedFolders.contains(Folder))
  {
    GSealedFolders.append(Folder);
  }
}

}
