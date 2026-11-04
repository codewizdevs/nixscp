#include "mobile/KeyStore.h"

#include "core/ConfigVault.h"
#include "core/PuttyKey.h"

#include <QDateTime>
#include <QDir>
#include <QRandomGenerator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>

#include <libssh/libssh.h>

namespace
{
QString PublicLine(ssh_key Key, const QString & Comment)
{
  char * Base64 = nullptr;
  if (ssh_pki_export_pubkey_base64(Key, &Base64) != SSH_OK)
  {
    return QString();
  }
  QString Line = QStringLiteral("%1 %2 %3").arg(QString::fromLatin1(ssh_key_type_to_char(ssh_key_type(Key))),
    QString::fromLatin1(Base64), Comment);
  ssh_string_free_char(Base64);
  return Line.trimmed();
}
}

KeyStore::KeyStore(QObject * Parent) : QObject(Parent)
{
  QDir().mkpath(folder());
  QFile::setPermissions(folder(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
}

QString KeyStore::folder() const
{
  return Folder();
}

QString KeyStore::Folder()
{
  return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/keys");
}

namespace
{
QString TemporaryFolder()
{
  return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/keys");
}

void WritePrivate(const QString & Path, const QByteArray & Data)
{
  QFile File(Path);
  if (File.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    File.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    File.write(Data);
  }
}
}

void KeyStore::SealAll()
{
  if (!ConfigVault::Enabled())
  {
    return;
  }
  const QFileInfoList Files = QDir(Folder()).entryInfoList(QDir::Files);
  for (const QFileInfo & Info : Files)
  {
    if ((Info.suffix() == QStringLiteral("pub")) || ConfigVault::IsSealedFile(Info.filePath()))
    {
      continue;
    }
    QFile File(Info.filePath());
    if (File.open(QIODevice::ReadOnly))
    {
      QByteArray Plain = File.readAll();
      File.close();
      WritePrivate(Info.filePath(), ConfigVault::Seal(Plain));
    }
  }
}

QString KeyStore::PlainKeyFile(const QString & Path)
{
  if (!ConfigVault::IsSealedFile(Path))
  {
    return Path;
  }
  bool Locked = false;
  QByteArray Plain = ConfigVault::ReadFile(Path, &Locked);
  if (Locked || Plain.isEmpty())
  {
    return QString();
  }
  QDir().mkpath(TemporaryFolder());
  QFile::setPermissions(TemporaryFolder(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
  // the name stays (PuTTY keys are recognized by it), in a folder of its own
  QString Folder = TemporaryFolder() + QLatin1Char('/') +
    QString::number(QDateTime::currentMSecsSinceEpoch()) + QString::number(QRandomGenerator::global()->generate());
  QDir().mkpath(Folder);
  QString Temp = Folder + QLatin1Char('/') + QFileInfo(Path).fileName();
  WritePrivate(Temp, Plain);
  return Temp;
}

void KeyStore::RemoveTemporary(const QString & Path)
{
  if (Path.startsWith(TemporaryFolder() + QLatin1Char('/')))
  {
    QDir(QFileInfo(Path).absolutePath()).removeRecursively();
  }
}

void KeyStore::CleanTemporary()
{
  QDir(TemporaryFolder()).removeRecursively();
}

QString KeyStore::path(const QString & Name) const
{
  return Name.isEmpty() ? QString() : (folder() + QLatin1Char('/') + Name);
}

QString KeyStore::nameOf(const QString & Path) const
{
  QFileInfo Info(Path);
  return (QDir(Info.absolutePath()) == QDir(folder())) ? Info.fileName() : QString();
}

QVariantList KeyStore::keys() const
{
  QVariantList Result;
  const QFileInfoList Files = QDir(folder()).entryInfoList(QDir::Files, QDir::Name);
  for (const QFileInfo & Info : Files)
  {
    if (Info.suffix() == QStringLiteral("pub"))
    {
      continue;
    }
    Result.append(QVariantMap{{QStringLiteral("name"), Info.fileName()}, {QStringLiteral("path"), Info.filePath()}});
  }
  return Result;
}

QString KeyStore::UniqueName(const QString & Wanted) const
{
  QString Base = QFileInfo(Wanted).fileName();
  Base.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._@-]")), QStringLiteral("_"));
  if (Base.isEmpty() || Base.startsWith(QLatin1Char('.')))
  {
    Base = QStringLiteral("key") + Base;
  }
  QString Name = Base;
  for (int I = 2; QFileInfo::exists(path(Name)) || QFileInfo::exists(path(Name) + QStringLiteral(".pub")); I++)
  {
    Name = QStringLiteral("%1-%2").arg(Base).arg(I);
  }
  return Name;
}

QString KeyStore::importKey(const QString & Source)
{
  QUrl Url(Source);
  QString From = Url.isLocalFile() ? Url.toLocalFile() : Source;
  QFile File(From);
  if (!File.open(QIODevice::ReadOnly))
  {
    FLastError = QStringLiteral("Cannot read the key file.");
    return QString();
  }
  QByteArray Data = File.read(256 * 1024);
  File.close();
  bool Putty = Data.startsWith("PuTTY-User-Key-File-");
  bool OpenSsh = Data.contains("PRIVATE KEY-----");
  if (!Putty && !OpenSsh)
  {
    FLastError = QStringLiteral("This is not a private key (PuTTY .ppk or OpenSSH).");
    return QString();
  }
  // content:// URLs end with a document id, not always the file's name
  QString Wanted = QFileInfo(From).fileName();
  if (Wanted.isEmpty() || Wanted.contains(QLatin1Char(':')))
  {
    Wanted = Putty ? QStringLiteral("key.ppk") : QStringLiteral("id_key");
  }
  QString Name = UniqueName(Wanted);
  QFile Target(path(Name));
  if (!Target.open(QIODevice::WriteOnly) || (Target.write(Data) != Data.size()))
  {
    FLastError = QStringLiteral("Cannot store the key.");
    return QString();
  }
  Target.close();
  Target.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  SealAll();
  emit changed();
  return Name;
}

QString KeyStore::generate(const QString & Wanted, const QString & Type, const QString & Passphrase)
{
  ssh_key Key = nullptr;
  bool Rsa = (Type == QStringLiteral("rsa"));
  if (ssh_pki_generate(Rsa ? SSH_KEYTYPE_RSA : SSH_KEYTYPE_ED25519, Rsa ? 4096 : 0, &Key) != SSH_OK)
  {
    FLastError = QStringLiteral("The key could not be generated.");
    return QString();
  }
  QString Name = UniqueName(Wanted.trimmed().isEmpty() ? (Rsa ? QStringLiteral("id_rsa") : QStringLiteral("id_ed25519")) : Wanted.trimmed());
  QByteArray File = QFile::encodeName(path(Name));
  QByteArray Pass = Passphrase.toUtf8();
  int Rc = ssh_pki_export_privkey_file(Key, Passphrase.isEmpty() ? nullptr : Pass.constData(), nullptr, nullptr,
    File.constData());
  QString Public = PublicLine(Key, Name + QStringLiteral("@nixscp-android"));
  ssh_key_free(Key);
  if (Rc != SSH_OK)
  {
    FLastError = QStringLiteral("The key could not be saved.");
    QFile::remove(path(Name));
    return QString();
  }
  QFile::setPermissions(path(Name), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  QFile Pub(path(Name) + QStringLiteral(".pub"));
  if (Pub.open(QIODevice::WriteOnly))
  {
    Pub.write(Public.toUtf8() + '\n');
  }
  SealAll();
  emit changed();
  return Name;
}

QString KeyStore::publicKey(const QString & Name) const
{
  QFile Pub(path(Name) + QStringLiteral(".pub"));
  if (Pub.open(QIODevice::ReadOnly))
  {
    return QString::fromUtf8(Pub.readAll()).trimmed();
  }
  ssh_key Key = nullptr;
  // an encrypted key: from a decrypted temporary copy
  QString File = PlainKeyFile(path(Name));
  if (File.isEmpty())
  {
    return QString();
  }
  struct Cleanup
  {
    QString Path;
    ~Cleanup() { RemoveTemporary(Path); }
  } Temp{File};
  if (PuttyKey::IsPuttyKeyFile(File))
  {
    // without asking: an encrypted key's public part needs the passphrase here
    if (PuttyKey::Load(File, [](QString &, bool) { return false; }, &Key, nullptr) != PuttyKey::lrOk)
    {
      return QString();
    }
  }
  else if (ssh_pki_import_privkey_file(QFile::encodeName(File).constData(), nullptr, nullptr, nullptr, &Key) != SSH_OK)
  {
    return QString();
  }
  QString Line = PublicLine(Key, Name);
  ssh_key_free(Key);
  return Line;
}

void KeyStore::remove(const QString & Name)
{
  if (Name.isEmpty() || Name.contains(QLatin1Char('/')))
  {
    return;
  }
  QFile::remove(path(Name));
  QFile::remove(path(Name) + QStringLiteral(".pub"));
  emit changed();
}
