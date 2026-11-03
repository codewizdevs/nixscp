#include "core/CloudSettings.h"

#include <QCryptographicHash>
#include <QStringList>

#include <algorithm>

namespace CloudSettings
{

static bool IsSectionOrBelow(const QString & Section, const QString & Parent)
{
  return (Section.compare(Parent, Qt::CaseInsensitive) == 0) ||
    Section.startsWith(Parent + QLatin1Char('\\'), Qt::CaseInsensitive);
}

QString AutoWorkspacePrefix(bool AutoSaveWorkspace, const QString & WorkspaceName)
{
  if (!AutoSaveWorkspace || WorkspaceName.isEmpty())
  {
    return QString();
  }
  // As StoredSessionList::Save names the sections ("Sessions\" + MungeStr(Name))
  return QStringLiteral("Sessions\\") + MungeStr(WorkspaceName + QLatin1Char('/'), false);
}

bool IsDeviceLocalSection(const QString & Section, const QString & AutoWorkspacePrefix)
{
  // The root section holds the machine-wide policies, never the user's settings
  if (Section.isEmpty())
  {
    return true;
  }
  // CDCache: the remote directory cache, rewritten while browsing
  if (IsSectionOrBelow(Section, KeySection) ||
      IsSectionOrBelow(Section, QStringLiteral("Configuration\\CDCache")) ||
      IsSectionOrBelow(Section, QStringLiteral("Configuration\\History")) ||
      IsSectionOrBelow(Section, QStringLiteral("Configuration\\Interface\\Commander\\LocalPanel")) ||
      IsSectionOrBelow(Section, QStringLiteral("Configuration\\Interface\\Commander\\RemotePanel")))
  {
    return true;
  }
  // Team sites ("Team: <name>/..."): synced with the team (core/TeamSync), not personally
  if (Section.startsWith(QStringLiteral("Sessions\\") + MungeStr(TeamFolderPrefix, false), Qt::CaseInsensitive))
  {
    return true;
  }
  return !AutoWorkspacePrefix.isEmpty() && Section.startsWith(AutoWorkspacePrefix, Qt::CaseInsensitive);
}

bool IsDeviceLocalKey(const QString & Key)
{
  static const QStringList Keys
  {
    QStringLiteral("WindowGeometry"), QStringLiteral("WindowState"), QStringLiteral("WindowSize"),
    QStringLiteral("PanelSplitter"), QStringLiteral("Height"), QStringLiteral("HeightPixelsPerInch"),
    QStringLiteral("LastLocalDirectory"), QStringLiteral("LastStoredSession"), QStringLiteral("UpdateLastCheck"),
    QStringLiteral("OpenedStoredSessionFolders"),
  };
  return Keys.contains(Key, Qt::CaseInsensitive);
}

IniFile Syncable(const IniFile & File, const QString & AutoWorkspacePrefix)
{
  IniFile Result;
  for (const QString & Section : File.Sections())
  {
    if (IsDeviceLocalSection(Section, AutoWorkspacePrefix))
    {
      continue;
    }
    Result.AddSection(Section);
    for (const QString & Key : File.Keys(Section))
    {
      if (!IsDeviceLocalKey(Key))
      {
        Result.WriteRaw(Section, Key, File.ReadRaw(Section, Key));
      }
    }
  }
  return Result;
}

static void SortCaseInsensitive(QStringList & List)
{
  std::sort(List.begin(), List.end(), [](const QString & A, const QString & B)
  {
    int Result = A.compare(B, Qt::CaseInsensitive);
    return (Result != 0) ? (Result < 0) : (A < B);
  });
}

QByteArray Canonical(const IniFile & File)
{
  QStringList Sections = File.Sections();
  SortCaseInsensitive(Sections);
  QByteArray Result;
  for (const QString & Section : Sections)
  {
    if (!Result.isEmpty())
    {
      Result += '\n';
    }
    Result += '[' + Section.toUtf8() + "]\n";
    QStringList Keys = File.Keys(Section);
    SortCaseInsensitive(Keys);
    for (const QString & Key : Keys)
    {
      Result += Key.toUtf8() + '=' + File.ReadRaw(Section, Key).toUtf8() + '\n';
    }
  }
  return Result;
}

QString Hash(const QByteArray & Data)
{
  return QString::fromLatin1(QCryptographicHash::hash(Data, QCryptographicHash::Sha256).toHex());
}

IniFile MergePulled(const IniFile & Remote, const IniFile & Local, const QString & AutoWorkspacePrefix)
{
  IniFile Result = Syncable(Remote, AutoWorkspacePrefix);
  for (const QString & Section : Local.Sections())
  {
    if (IsDeviceLocalSection(Section, AutoWorkspacePrefix))
    {
      if (!Section.isEmpty())
      {
        Result.AddSection(Section);
        for (const QString & Key : Local.Keys(Section))
        {
          Result.WriteRaw(Section, Key, Local.ReadRaw(Section, Key));
        }
      }
      continue;
    }
    for (const QString & Key : Local.Keys(Section))
    {
      if (IsDeviceLocalKey(Key))
      {
        Result.WriteRaw(Section, Key, Local.ReadRaw(Section, Key));
      }
    }
  }
  return Result;
}

}
