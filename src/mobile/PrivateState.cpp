#include "mobile/PrivateState.h"

#include "core/ConfigVault.h"

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

QString PrivateState::DefaultFile()
{
  return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/sites.dat");
}

void PrivateState::Load()
{
  bool Locked = false;
  QByteArray Plain = ConfigVault::ReadFile(FFileName, &Locked);
  // encrypted with a key that is gone: the lists start over
  FData = Locked ? QVariantMap() : QJsonDocument::fromJson(Plain).object().toVariantMap();
  FLoaded = true;
}

void PrivateState::Reseal()
{
  if (FLoaded && QFile::exists(FFileName) && (ConfigVault::Enabled() != ConfigVault::IsSealedFile(FFileName)))
  {
    Save();
  }
}

QStringList PrivateState::Strings(const QString & Key) const
{
  return FData.value(Key).toStringList();
}

bool PrivateState::SetStrings(const QString & Key, const QStringList & Value)
{
  if (Value.isEmpty())
  {
    FData.remove(Key);
  }
  else
  {
    FData.insert(Key, Value);
  }
  return Save();
}

bool PrivateState::Save()
{
  // locked: nothing was read, so nothing may replace the file
  if (!FLoaded)
  {
    return false;
  }
  QDir().mkpath(QFileInfo(FFileName).absolutePath());
  QByteArray Plain = QJsonDocument(QJsonObject::fromVariantMap(FData)).toJson(QJsonDocument::Compact);
  QSaveFile File(FFileName);
  if (!File.open(QIODevice::WriteOnly))
  {
    return false;
  }
  File.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  QByteArray Data = ConfigVault::Enabled() ? ConfigVault::Seal(Plain) : Plain;
  return (File.write(Data) == Data.size()) && File.commit();
}

//---------------------------------------------------------------------------
namespace MobileCache
{
namespace
{
const QStringList Kinds = {QStringLiteral("edit"), QStringLiteral("open"), QStringLiteral("upload"), QStringLiteral("archive"),
  QStringLiteral("thumbfetch")};

QString SharedThumbnails()
{
  return Root() + QStringLiteral("/thumbs/shared");
}

QString SessionThumbnails(int Session)
{
  return Root() + QStringLiteral("/thumbs/%1").arg(Session);
}
}

QString Root()
{
  return QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
}

QString Folder(const QString & Kind, int Session)
{
  return Root() + QLatin1Char('/') + Kind + QLatin1Char('/') + QString::number(Session);
}

QString Thumbnails(int Session)
{
  return ConfigVault::Enabled() ? SessionThumbnails(Session) : SharedThumbnails();
}

void RemoveSession(int Session)
{
  for (const QString & Kind : Kinds)
  {
    QDir(Folder(Kind, Session)).removeRecursively();
  }
  QDir(SessionThumbnails(Session)).removeRecursively();
}

void Clean(bool Encrypted)
{
  for (const QString & Kind : Kinds)
  {
    QDir(Root() + QLatin1Char('/') + Kind).removeRecursively();
  }
  QDir Thumbs(Root() + QStringLiteral("/thumbs"));
  const QStringList Sessions = Thumbs.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
  for (const QString & Name : Sessions)
  {
    if (Encrypted || (Name != QStringLiteral("shared")))
    {
      QDir(Thumbs.filePath(Name)).removeRecursively();
    }
  }
}

void RemoveSharedThumbnails()
{
  QDir(SharedThumbnails()).removeRecursively();
}
}
