#include "mobile/KeyStore.h"

#include "core/ConfigVault.h"
#include "core/PuttyKey.h"

#include <QCoreApplication>
#include <QDir>
#include <QMessageAuthenticationCode>
#include <QPointer>
#include <QRandomGenerator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>
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
// all of it or nothing: the file is replaced only by a complete new one, so
// a kill or full storage cannot take the only copy of a key
bool WritePrivate(const QString & Path, const QByteArray & Data)
{
  QSaveFile File(Path);
  if (!File.open(QIODevice::WriteOnly))
  {
    return false;
  }
  File.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  return (File.write(Data) == Data.size()) && File.commit();
}

// what is stored for a private key: encrypted when the key is known
QByteArray ForStorage(const QByteArray & Plain)
{
  return ConfigVault::Enabled() ? ConfigVault::Seal(Plain) : Plain;
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

void KeyStore::HashKnownHosts(const QString & FileName)
{
  QFile File(FileName);
  if (!File.open(QIODevice::ReadOnly))
  {
    return;
  }
  const QList<QByteArray> Lines = File.readAll().split('\n');
  File.close();
  QByteArray Result;
  bool Changed = false;
  for (const QByteArray & Line : Lines)
  {
    qsizetype Space = Line.indexOf(' ');
    // comments, markers and what is hashed already stay
    if (Line.isEmpty() || (Space <= 0) || Line.startsWith('#') || Line.startsWith('@') || Line.startsWith('|'))
    {
      if (!Line.isEmpty())
      {
        Result += Line + '\n';
      }
      continue;
    }
    Changed = true;
    const QList<QByteArray> Hosts = Line.left(Space).split(',');
    for (const QByteArray & Host : Hosts)
    {
      QByteArray Salt(20, Qt::Uninitialized);
      QRandomGenerator::system()->generate(reinterpret_cast<quint32 *>(Salt.data()), reinterpret_cast<quint32 *>(Salt.data() + Salt.size()));
      QByteArray Hash = QMessageAuthenticationCode::hash(Host, Salt, QCryptographicHash::Sha1);
      Result += "|1|" + Salt.toBase64() + '|' + Hash.toBase64() + Line.mid(Space) + '\n';
    }
  }
  if (Changed)
  {
    WritePrivate(FileName, Result);
  }
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
  // encrypted before it is written: no plain copy on the phone
  if (!WritePrivate(path(Name), ForStorage(Data)))
  {
    FLastError = QStringLiteral("Cannot store the key.");
    return QString();
  }
  emit changed();
  return Name;
}

void KeyStore::generate(const QString & Wanted, const QString & Type, const QString & Passphrase)
{
  if (FGenerating)
  {
    return;
  }
  bool Rsa = (Type == QStringLiteral("rsa"));
  QString Name = UniqueName(Wanted.trimmed().isEmpty() ? (Rsa ? QStringLiteral("id_rsa") : QStringLiteral("id_ed25519")) : Wanted.trimmed());
  FGenerating = true;
  emit generatingChanged();
  QPointer<KeyStore> Self(this);
  // RSA 4096 takes many seconds on a phone: not on the GUI thread
  QThread * Worker = QThread::create([Self, Rsa, Name, Passphrase]()
  {
    QByteArray Private;
    QString Public;
    QString Error;
    ssh_key Key = nullptr;
    if (ssh_pki_generate(Rsa ? SSH_KEYTYPE_RSA : SSH_KEYTYPE_ED25519, Rsa ? 4096 : 0, &Key) != SSH_OK)
    {
      Error = QStringLiteral("The key could not be generated.");
    }
    else
    {
      QByteArray Pass = Passphrase.toUtf8();
      char * Base64 = nullptr;
      if (ssh_pki_export_privkey_base64(Key, Passphrase.isEmpty() ? nullptr : Pass.constData(), nullptr, nullptr, &Base64) == SSH_OK)
      {
        Private = QByteArray(Base64);
        ssh_string_free_char(Base64);
        Public = PublicLine(Key, Name + QStringLiteral("@nixscp-android"));
      }
      else
      {
        Error = QStringLiteral("The key could not be saved.");
      }
      ssh_key_free(Key);
    }
    QMetaObject::invokeMethod(QCoreApplication::instance(), [Self, Name, Private, Public, Error]()
    {
      if (Self != nullptr)
      {
        Self->Generated(Name, Private, Public, Error);
      }
    }, Qt::QueuedConnection);
  });
  connect(Worker, &QThread::finished, Worker, &QObject::deleteLater);
  Worker->start();
}

void KeyStore::Generated(const QString & Wanted, const QByteArray & Private, const QString & Public, const QString & Error)
{
  FGenerating = false;
  emit generatingChanged();
  if (!Error.isEmpty())
  {
    emit generated(QString(), Error);
    return;
  }
  // another key may have taken the name meanwhile
  QString Name = UniqueName(Wanted);
  if (!WritePrivate(path(Name), ForStorage(Private)))
  {
    emit generated(QString(), QStringLiteral("The key could not be saved."));
    return;
  }
  QFile Pub(path(Name) + QStringLiteral(".pub"));
  if (Pub.open(QIODevice::WriteOnly))
  {
    Pub.write(Public.toUtf8() + '\n');
    // complete before anyone reads it: the page shows it right away
    Pub.close();
  }
  emit changed();
  emit generated(Name, QString());
}

QString KeyStore::publicKey(const QString & Name) const
{
  QFile Pub(path(Name) + QStringLiteral(".pub"));
  if (Pub.open(QIODevice::ReadOnly))
  {
    QString Line = QString::fromUtf8(Pub.readAll()).trimmed();
    if (!Line.isEmpty())
    {
      return Line;
    }
  }
  // an encrypted key is opened in memory
  bool Locked = false;
  QByteArray Data = ConfigVault::ReadFile(path(Name), &Locked);
  if (Locked || Data.isEmpty())
  {
    return QString();
  }
  ssh_key Key = nullptr;
  if (Data.startsWith("PuTTY-User-Key-File-"))
  {
    // without asking: an encrypted key's public part needs the passphrase here
    if (PuttyKey::LoadData(Data, [](QString &, bool) { return false; }, &Key, nullptr) != PuttyKey::lrOk)
    {
      return QString();
    }
  }
  else if (ssh_pki_import_privkey_base64(Data.constData(), nullptr, nullptr, nullptr, &Key) != SSH_OK)
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
