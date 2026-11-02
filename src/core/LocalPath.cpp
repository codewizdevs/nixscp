#include "core/LocalPath.h"
#include "core/RemoteFile.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

namespace LocalPath
{

#ifdef Q_OS_WIN

QString Root(const QString & Path)
{
  static const QRegularExpression Drive(QStringLiteral("^[A-Za-z]:(/|$)"));
  static const QRegularExpression Unc(QStringLiteral("^//[^/]+/[^/]+(/|$)"));
  if (QRegularExpressionMatch M = Drive.match(Path); M.hasMatch())
  {
    return Path.left(2).toUpper() + QLatin1Char('/');
  }
  if (QRegularExpressionMatch M = Unc.match(Path); M.hasMatch())
  {
    QString Result = M.captured(0);
    return Result.endsWith(QLatin1Char('/')) ? Result : (Result + QLatin1Char('/'));
  }
  return Path.startsWith(QLatin1Char('/')) ? QStringLiteral("/") : QString();
}

bool IsRoot(const QString & Path)
{
  QString R = Root(Path);
  if (R.isEmpty())
  {
    return false;
  }
  QString P = Path.endsWith(QLatin1Char('/')) ? Path : (Path + QLatin1Char('/'));
  return P.compare(R, Qt::CaseInsensitive) == 0;
}

QString Parent(const QString & Path)
{
  QString R = Root(Path);
  if (R.isEmpty() || IsRoot(Path))
  {
    return R.isEmpty() ? Path : R;
  }
  QString P = Path;
  while (P.endsWith(QLatin1Char('/')))
  {
    P.chop(1);
  }
  QString Result = P.left(P.lastIndexOf(QLatin1Char('/')));
  // "C:/users" -> "C:" and "//server/share/dir" -> "//server/share": the root
  return (Result.size() + 1 <= R.size()) ? R : Result;
}

QString FileName(const QString & Path)
{
  if (IsRoot(Path))
  {
    return QString();
  }
  return UnixPath::ExtractFileName(Path);
}

QString Display(const QString & Path)
{
  return QDir::toNativeSeparators(Path);
}

Qt::CaseSensitivity Case()
{
  return Qt::CaseInsensitive;
}

QStringList Roots()
{
  QStringList Result;
  for (const QFileInfo & Drive : QDir::drives())
  {
    Result.append(Root(Drive.absoluteFilePath()));
  }
  return Result;
}

#else

QString Root(const QString & Path)
{
  return Path.startsWith(QLatin1Char('/')) ? QStringLiteral("/") : QString();
}

bool IsRoot(const QString & Path)
{
  return UnixPath::IsRoot(Path);
}

QString Parent(const QString & Path)
{
  return UnixPath::Parent(Path);
}

QString FileName(const QString & Path)
{
  return UnixPath::ExtractFileName(Path);
}

QString Display(const QString & Path)
{
  return Path;
}

Qt::CaseSensitivity Case()
{
  return Qt::CaseSensitive;
}

QStringList Roots()
{
  return {QStringLiteral("/")};
}

#endif

}
