// New version check: asks GitHub for the latest NixSCP release (in the
// background, asynchronously) and tells the user when it is newer than this
// version. Nothing is downloaded or installed: the notice links to the
// release page. Preferences > Updates turns it off.
#pragma once

#include <QString>

class QWidget;

class QDateTime;

namespace UpdateCheck
{
  // Shortly after startup, at most once a day, when enabled
  void Start(QWidget * Window);
  // Preferences > Updates "Check now": reports the result in any case
  void CheckNow(QWidget * Window);
  // This version ("1.2.0")
  QString CurrentVersion();
  // Latest is newer than Current ("1.2.0" or "v1.2.0")
  bool IsNewer(const QString & Latest, const QString & Current);
  // Time for the daily check? A last check in the future (clock changed) does not count
  bool CheckDue(const QDateTime & LastCheck, const QDateTime & Now);
}
