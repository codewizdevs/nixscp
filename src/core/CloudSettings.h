// What of nixscp.ini is synced by NixSCP Pro (nixscp-cloud docs/SYNC.md,
// "Normalization"). Window layout, histories, caches, local paths and the auto-saved
// workspace (open tabs travel in the "session" slot) stay on each computer:
// they are left out of the uploaded settings, and kept when settings from
// another computer are applied.
#pragma once

#include "core/IniStorage.h"

#include <QByteArray>
#include <QString>

namespace CloudSettings
{
  // Optional section of an exported configuration with the cloud sync data key
  // (CloudSync::ExportKey). Outside WinSCP's "Configuration" and "Sessions"
  // trees, values in hex: WinSCP ignores it. Never kept in nixscp.ini, never synced.
  inline const QString KeySection = QStringLiteral("NixSCP\\CloudSync");

  // "Sessions\<munged workspace>/" for the workspace that AutoSaveWorkspace
  // rewrites on every exit; empty when nothing is auto-saved
  QString AutoWorkspacePrefix(bool AutoSaveWorkspace, const QString & WorkspaceName);

  bool IsDeviceLocalSection(const QString & Section, const QString & AutoWorkspacePrefix);
  bool IsDeviceLocalKey(const QString & Key);

  // File without the device-local sections and keys
  IniFile Syncable(const IniFile & File, const QString & AutoWorkspacePrefix);
  // Sections and keys sorted case-insensitively, raw values, LF line endings:
  // equal settings give equal bytes on every computer
  QByteArray Canonical(const IniFile & File);
  // Lower-case hex SHA-256
  QString Hash(const QByteArray & Data);

  // Settings pulled from the cloud with this computer's device-local part
  IniFile MergePulled(const IniFile & Remote, const IniFile & Local, const QString & AutoWorkspacePrefix);
}
