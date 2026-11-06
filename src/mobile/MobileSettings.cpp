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
  // the transfer confirmations live in the configuration's copy settings;
  // a cloud pull brings another device's, the phone's stay over them
  Configuration & Config = Configuration::Instance();
  Config.DefaultCopyParam().ConfirmOverwriting = confirmOverwrite();
  connect(&Config, &Configuration::Changed, this, &MobileSettings::ApplyToConfiguration, Qt::UniqueConnection);
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
  return FPrivate.Strings(QStringLiteral("folders/") + Site);
}

void MobileSettings::addRecentFolder(const QString & Site, const QString & Folder)
{
  if (Site.isEmpty() || Folder.isEmpty())
  {
    return;
  }
  QString Key = QStringLiteral("folders/") + Site;
  QStringList List = FPrivate.Strings(Key);
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
  FPrivate.SetStrings(Key, List);
}

QString MobileSettings::siteKey(const QString & Site) const
{
  return FPrivate.Strings(QStringLiteral("key/") + Site).value(0);
}

void MobileSettings::setSiteKey(const QString & Site, const QString & KeyName)
{
  FPrivate.SetStrings(QStringLiteral("key/") + Site, KeyName.isEmpty() ? QStringList() : QStringList{KeyName});
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
