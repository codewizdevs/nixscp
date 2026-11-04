#include "mobile/MobileSettings.h"

#include "core/Configuration.h"
#include "core/SessionData.h"

#include <QStandardPaths>

MobileSettings::MobileSettings(QObject * Parent) :
  QObject(Parent),
  FSettings(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/mobile.ini"),
    QSettings::IniFormat)
{
}

void MobileSettings::ApplyToConfiguration()
{
  // the transfer confirmations live in the configuration's copy settings
  Configuration::Instance().DefaultCopyParam().ConfirmOverwriting = confirmOverwrite();
}

int MobileSettings::Int(const QString & Key, int Default) const
{
  return FSettings.value(QStringLiteral("Settings/") + Key, Default).toInt();
}

bool MobileSettings::Bool(const QString & Key, bool Default) const
{
  return FSettings.value(QStringLiteral("Settings/") + Key, Default).toBool();
}

QString MobileSettings::String(const QString & Key, const QString & Default) const
{
  return FSettings.value(QStringLiteral("Settings/") + Key, Default).toString();
}

void MobileSettings::Set(const QString & Key, const QVariant & Value)
{
  if (FSettings.value(QStringLiteral("Settings/") + Key) == Value)
  {
    return;
  }
  FSettings.setValue(QStringLiteral("Settings/") + Key, Value);
  FSettings.sync();
  emit changed();
}

void MobileSettings::setConfirmOverwrite(bool Value)
{
  Configuration::Instance().DefaultCopyParam().ConfirmOverwriting = Value;
  Set(QStringLiteral("ConfirmOverwrite"), Value);
}

QStringList MobileSettings::recentFolders(const QString & Site) const
{
  return FSettings.value(QStringLiteral("RecentFolders/") + QString::fromLatin1(Site.toUtf8().toBase64(QByteArray::Base64UrlEncoding))).toStringList();
}

void MobileSettings::addRecentFolder(const QString & Site, const QString & Folder)
{
  if (Site.isEmpty() || Folder.isEmpty())
  {
    return;
  }
  // the site's name as a key (it may hold '/' and spaces)
  QString Key = QStringLiteral("RecentFolders/") + QString::fromLatin1(Site.toUtf8().toBase64(QByteArray::Base64UrlEncoding));
  QStringList List = FSettings.value(Key).toStringList();
  if (!List.isEmpty() && (List.first() == Folder))
  {
    return;
  }
  List.removeAll(Folder);
  List.prepend(Folder);
  while (List.size() > 10)
  {
    List.removeLast();
  }
  FSettings.setValue(Key, List);
}

namespace
{
QString SiteKeyName(const QString & Site)
{
  return QStringLiteral("SiteKeys/") + QString::fromLatin1(Site.toUtf8().toBase64(QByteArray::Base64UrlEncoding));
}
}

QString MobileSettings::siteKey(const QString & Site) const
{
  return FSettings.value(SiteKeyName(Site)).toString();
}

void MobileSettings::setSiteKey(const QString & Site, const QString & KeyName)
{
  if (KeyName.isEmpty())
  {
    FSettings.remove(SiteKeyName(Site));
  }
  else
  {
    FSettings.setValue(SiteKeyName(Site), KeyName);
  }
  FSettings.sync();
}

void MobileSettings::Apply(SessionData & Data) const
{
  Data.Timeout = timeout();
  int Keepalive = keepalive();
  if (Keepalive > 0)
  {
    Data.PingType = ptNullPacket;
    Data.PingInterval = Keepalive;
    Data.FtpPingType = fptDummyCommand;
    Data.FtpPingInterval = Keepalive;
  }
  else
  {
    Data.PingType = ptOff;
    Data.FtpPingType = fptOff;
  }
}
