// What a NixSCP team shares: the content of its snapshots. A canonical INI text
// with the team's name and its sites, reduced to an allow-list of settings on
// push and on apply. Anything that runs a command, weakens a check or reads a
// local file stays each person's own: a team snapshot sets none of it. A new
// SessionData setting is not shared until it is added to the table.
#pragma once

#include "core/IniStorage.h"

#include <QByteArray>
#include <QHash>
#include <QString>

#include <memory>

class SessionData;

namespace TeamContent
{
  // Whitespace runs to one space, no control characters or markup, at most 100 characters.
  // The one place a team name is normalized: every device gets this result.
  QString NormalizeName(const QString & Name);

  // One fold for everything that compares names: exactly what QString::compare
  // with Qt::CaseInsensitive, and so StoredSessionList, takes to be the same
  QString Fold(const QString & Text);
  // Names people could take for one another: the fold, compatibility forms
  // ("Ｐｒｏｄ"), marks, and letters of other scripts that look Latin
  QString Skeleton(const QString & Text);

  // The setting may travel in a team snapshot
  bool IsSharedKey(const QString & Key);

  // The section of a site ("sub/site", relative to the team's folder)
  QString SiteSection(const QString & Relative);
  QString SiteName(const QString & Section);
  // Adds a site, with the shared settings only (the password decoded, so equal
  // sites give equal bytes); false when the site cannot be shared
  bool AddSite(IniFile & Sites, const QString & Relative, const SessionData & Data);

  // The bytes of a snapshot's content
  QByteArray Encode(const QString & TeamName, const IniFile & Sites);
  // Reads a snapshot's content keeping only valid sites and shared settings
  // (Dropped: how many settings or sites were refused). False when it is not team content.
  bool Decode(const QByteArray & Content, QString & TeamName, IniFile & Sites, int * Dropped = nullptr);

  // Hash of all the sites, and one per site (relative name to hash)
  QString Hash(const IniFile & Sites);
  QHash<QString, QString> SiteHashes(const IniFile & Sites);

  // The site of Section with this person's own settings of Local (may be null)
  std::shared_ptr<SessionData> Merge(const IniFile & Sites, const QString & Section, const SessionData * Local, const QString & Name);
}
