// Paths of local files, as the panels use them: always with '/' separators
// (Qt's convention, also on Windows), but with the roots of the platform:
// "/" on Linux; "C:/" and "//server/share/" on Windows, where paths are shown
// with backslashes and compared case-insensitively.
#pragma once

#include <QString>
#include <QStringList>

namespace LocalPath
{
  // The root of Path: "/", "C:/" or "//server/share/" (empty when relative)
  QString Root(const QString & Path);
  bool IsRoot(const QString & Path);
  // The parent directory; the root is its own parent
  QString Parent(const QString & Path);
  QString FileName(const QString & Path);
  // For the user: backslashes on Windows
  QString Display(const QString & Path);
  Qt::CaseSensitivity Case();
  // Drive roots ("C:/", "D:/"...) on Windows; "/" on Linux
  QStringList Roots();
}
