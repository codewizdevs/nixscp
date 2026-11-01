// Port of WinSCP source/core/RemoteFiles.cpp (subset)
#include "core/RemoteFile.h"

QString Rights::ToText(unsigned int Mode)
{
  // TRights::GetText, including setuid/setgid/sticky representation
  QString Result(9, QLatin1Char('-'));
  const char * Letters = "rwxrwxrwx";
  for (int I = 0; I < 9; I++)
  {
    if (Mode & (1u << (8 - I)))
    {
      Result[I] = QLatin1Char(Letters[I]);
    }
  }
  if (Mode & 04000) Result[2] = (Mode & 0100) ? QLatin1Char('s') : QLatin1Char('S');
  if (Mode & 02000) Result[5] = (Mode & 0010) ? QLatin1Char('s') : QLatin1Char('S');
  if (Mode & 01000) Result[8] = (Mode & 0001) ? QLatin1Char('t') : QLatin1Char('T');
  return Result;
}

QString Rights::ToOctal(unsigned int Mode)
{
  // As in WinSCP Properties dialog, always 4 digits
  return QStringLiteral("%1").arg(Mode & 07777, 4, 8, QLatin1Char('0'));
}

bool Rights::FromOctal(const QString & Str, unsigned int & Mode)
{
  if (Str.isEmpty() || (Str.size() > 4))
  {
    return false;
  }
  bool Ok = false;
  unsigned int Value = Str.toUInt(&Ok, 8);
  if (Ok)
  {
    Mode = Value;
  }
  return Ok;
}

QString RemoteFile::RightsText() const
{
  return Rights::ToText(Permissions);
}

QString RemoteFile::TypeChar() const
{
  if (IsSymLink) return QStringLiteral("l");
  if (FileType == Directory) return QStringLiteral("d");
  return QStringLiteral("-");
}

namespace UnixPath
{

QString Combine(const QString & Dir, const QString & Name)
{
  if (Name.startsWith(QLatin1Char('/')))
  {
    return Name;
  }
  if (Dir.endsWith(QLatin1Char('/')))
  {
    return Dir + Name;
  }
  return Dir + QLatin1Char('/') + Name;
}

QString ExtractFileName(const QString & Path)
{
  int P = Path.lastIndexOf(QLatin1Char('/'));
  return Path.mid(P + 1);
}

QString ExtractFileDir(const QString & Path)
{
  int P = Path.lastIndexOf(QLatin1Char('/'));
  if (P == 0)
  {
    return QStringLiteral("/");
  }
  return (P > 0) ? Path.left(P) : QString();
}

QString Parent(const QString & Path)
{
  QString P = Path;
  while ((P.size() > 1) && P.endsWith(QLatin1Char('/')))
  {
    P.chop(1);
  }
  return IsRoot(P) ? P : ExtractFileDir(P);
}

bool IsRoot(const QString & Path)
{
  return Path == QStringLiteral("/");
}

}
