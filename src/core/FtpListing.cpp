// Port of WinSCP source/filezilla/FtpListResult.cpp (CFtpListResult) and of
// TFTPFileSystem::HandleListData from source/core/FtpFileSystem.cpp.
// The parser functions keep FileZilla's original structure and naming so they
// can be compared line by line with the original.
#include "core/FtpListing.h"

#include <QStringDecoder>
#include <QTimeZone>

#include <cstring>
#include <ctime>

namespace
{

void StrLwr(std::string & S)
{
  // strlwr in the "C" locale: ASCII only
  for (char & C : S)
  {
    if ((C >= 'A') && (C <= 'Z'))
    {
      C = static_cast<char>(C - 'A' + 'a');
    }
  }
}

int StrNICmp(const char * A, const char * B, size_t Len)
{
  for (size_t I = 0; I < Len; I++)
  {
    int CA = static_cast<unsigned char>(A[I]);
    int CB = static_cast<unsigned char>(B[I]);
    if ((CA >= 'A') && (CA <= 'Z')) CA += 'a' - 'A';
    if ((CB >= 'A') && (CB <= 'Z')) CB += 'a' - 'A';
    if (CA != CB)
    {
      return CA - CB;
    }
    if (CA == 0)
    {
      return 0;
    }
  }
  return 0;
}

}

FtpListParser::FtpListParser(bool Mlst, bool Utf8, int ServerType, bool VmsAllRevisions) :
  FMlst(Mlst),
  FUtf8(Utf8),
  FServerType(ServerType),
  FVmsAllRevisions(VmsAllRevisions),
  FCurrentDate(QDate::currentDate())
{
  // Keys are raw bytes, as the original converted each byte to a wide char
  // (so both Latin-1 and UTF-8 spellings are listed)
  static const struct { const char * Name; int Month; } MonthNames[] = {
    // English month names
    {"jan", 1}, {"feb", 2}, {"mar", 3}, {"apr", 4}, {"may", 5}, {"jun", 6}, {"june", 6},
    {"jul", 7}, {"july", 7}, {"aug", 8}, {"sep", 9}, {"sept", 9}, {"oct", 10}, {"nov", 11}, {"dec", 12},
    // Numerical values for the month
    {"1", 1}, {"01", 1}, {"2", 2}, {"02", 2}, {"3", 3}, {"03", 3}, {"4", 4}, {"04", 4},
    {"5", 5}, {"05", 5}, {"6", 6}, {"06", 6}, {"7", 7}, {"07", 7}, {"8", 8}, {"08", 8},
    {"9", 9}, {"09", 9}, {"10", 10}, {"11", 11}, {"12", 12},
    // German month names
    {"mrz", 3}, {"m\xE4r", 3}, {"m\xE4rz", 3}, {"mai", 5}, {"juni", 6},
    {"m\xC3\xA4r", 3}, {"m\xC3\xA4rz", 3}, {"juli", 7}, {"okt", 10}, {"dez", 12},
    // Austrian month names
    {"j\xE4n", 1}, {"j\xC3\xA4n", 1},
    // French month names
    {"janv", 1}, {"f\xE9" "b", 1}, {"f\xE9v", 2}, {"fev", 2}, {"f\xE9vr", 2}, {"fevr", 2},
    {"mars", 3}, {"mrs", 3}, {"avr", 4}, {"juin", 6}, {"juil", 7}, {"jui", 7},
    {"ao\xFB", 8}, {"ao\xFBt", 8}, {"aout", 8}, {"d\xE9" "c", 12},
    // Italian month names
    {"gen", 1}, {"mag", 5}, {"giu", 6}, {"lug", 7}, {"ago", 8}, {"set", 9}, {"ott", 10}, {"dic", 12},
    // Spanish month names
    {"ene", 1}, {"fbro", 2}, {"mzo", 3}, {"ab", 4}, {"abr", 4}, {"agto", 8},
    {"sbre", 9}, {"obre", 9}, {"nbre", 9}, {"dbre", 9},
    // Polish month names
    {"sty", 1}, {"lut", 2}, {"kwi", 4}, {"maj", 5}, {"cze", 6}, {"lip", 7}, {"sie", 8},
    {"wrz", 9}, {"pa", 10}, {"lis", 11}, {"gru", 12},
    // Russian month names (Windows-1251)
    {"\xFF\xED\xE2", 1}, {"\xF4\xE5\xE2", 2}, {"\xEC\xE0\xF0", 3}, {"\xE0\xEF\xF0", 4},
    {"\xEC\xE0\xE9", 5}, {"\xE8\xFE\xED", 6}, {"\xE8\xFE\xEB", 7}, {"\xE0\xE2\xE3", 8},
    {"\xF1\xE5\xED", 9}, {"\xEE\xEA\xF2", 10}, {"\xED\xEE\xFF", 11}, {"\xE4\xE5\xEA", 12},
    // Dutch month names
    {"mrt", 3}, {"mei", 5},
    // Portuguese month names
    {"out", 10},
    // Japanese month names
    {"1\x8c\x8e", 1}, {"2\x8c\x8e", 2}, {"3\x8c\x8e", 3}, {"4\x8c\x8e", 4}, {"5\x8c\x8e", 5}, {"6\x8c\x8e", 6},
    {"7\x8c\x8e", 7}, {"8\x8c\x8e", 8}, {"9\x8c\x8e", 9}, {"10\x8c\x8e", 10}, {"11\x8c\x8e", 11}, {"12\x8c\x8e", 12},
    // Korean (Unicode) month names
    {"1\xC6\xD4", 1}, {"2\xC6\xD4", 2}, {"3\xC6\xD4", 3}, {"4\xC6\xD4", 4}, {"5\xC6\xD4", 5}, {"6\xC6\xD4", 6},
    {"7\xC6\xD4", 7}, {"8\xC6\xD4", 8}, {"9\xC6\xD4", 9}, {"10\xC6\xD4", 10}, {"11\xC6\xD4", 11}, {"12\xC6\xD4", 12},
    // Korean (EUC-KR) month names
    {"1\xBF\xF9", 1}, {"2\xBF\xF9", 2}, {"3\xBF\xF9", 3}, {"4\xBF\xF9", 4}, {"5\xBF\xF9", 5}, {"6\xBF\xF9", 6},
    {"7\xBF\xF9", 7}, {"8\xBF\xF9", 8}, {"9\xBF\xF9", 9}, {"10\xBF\xF9", 10}, {"11\xBF\xF9", 11}, {"12\xBF\xF9", 12},
    // Finnish month names
    {"tammi", 1}, {"helmi", 2}, {"maalis", 3}, {"huhti", 4}, {"touko", 5}, {"kes\xE4", 6},
    {"hein\xE4", 7}, {"elo", 8}, {"syys", 9}, {"loka", 10}, {"marras", 11}, {"joulu", 12},
    // Some very strange combinations of names and numbers
    {"jan1", 1}, {"feb2", 2}, {"mar3", 3}, {"apr4", 4}, {"may5", 5}, {"jun6", 6}, {"jul7", 7},
    {"aug8", 8}, {"sep9", 9}, {"sept9", 9}, {"oct0", 10}, {"nov1", 11}, {"dec2", 12},
    // Slovenian month names
    {"avg", 8},
  };
  for (const auto & M : MonthNames)
  {
    FMonthNames[M.Name] = M.Month;
  }
}

int FtpListParser::FindMonth(const char * str, size_t len) const
{
  std::string Key(str, len);
  StrLwr(Key);
  auto iter = FMonthNames.find(Key);
  return (iter == FMonthNames.end()) ? 0 : iter->second;
}

QList<FtpListEntry> FtpListParser::GetList()
{
  if (!FBuffer.empty())
  {
    // Last line without line terminator
    AddData("\n", 1);
  }
  while (!FBuffer.empty() && IsNewLineChar(FBuffer.back()))
  {
    FBuffer.pop_back();
  }
  if (!FBuffer.empty())
  {
    FUnparsed.append(getStr(FBuffer.data(), FBuffer.size()));
  }
  FBuffer.clear();
  QList<FtpListEntry> Result = FEntries;
  FEntries.clear();
  FVersions.clear();
  return Result;
}

bool FtpListParser::ParseLine(const QByteArray & Line, FtpListEntry & Entry)
{
  std::string Buf(Line.constData(), static_cast<size_t>(Line.size()));
  return parseLine(Buf.c_str(), Buf.size(), Entry);
}

bool FtpListParser::parseLine(const char * lineToParse, const size_t linelen, FtpListEntry & direntry)
{
  direntry.OwnerGroup.clear();
  direntry.Owner.clear();
  direntry.Group.clear();

  if (parseAsMlsd(lineToParse, linelen, direntry))
    return true;

  if (parseAsUnix(lineToParse, linelen, direntry))
    return true;

  if (parseAsDos(lineToParse, linelen, direntry))
    return true;

  if (parseAsEPLF(lineToParse, linelen, direntry))
    return true;

  if (parseAsVMS(lineToParse, linelen, direntry))
  {
    FServerType |= stVMS;
    return true;
  }

  if (parseAsOther(lineToParse, linelen, direntry))
    return true;

  if (parseAsIBMMVS(lineToParse, linelen, direntry))
    return true;

  if (parseAsIBMMVSPDS(lineToParse, linelen, direntry))
    return true;

  if (parseAsIBM(lineToParse, linelen, direntry))
    return true;

  if (parseAsWfFtp(lineToParse, linelen, direntry))
    return true;

  // Should be last
  if (parseAsIBMMVSPDS2(lineToParse, linelen, direntry))
    return true;

  // name-only entries
  // (multiline VMS entries have only a name on the first line, so for VMS we have to skip this)
  if ((FServerType & stVMS) == 0)
  {
    std::string Buf(lineToParse, linelen);
    // z/OS PDS members without ISPF statistics (name only) still have loads of spaces after them.
    while (!Buf.empty() && (static_cast<unsigned char>(Buf.back()) <= ' '))
    {
      Buf.pop_back();
    }
    if (!Buf.empty() && (Buf.find(' ') == std::string::npos))
    {
      direntry = FtpListEntry();
      direntry.Name = getStr(Buf.data(), Buf.size());
      return true;
    }
  }

  return false;
}

bool FtpListParser::IsNewLineChar(char C)
{
  return
    (C == '\r') || (C == '\n') ||
    // Some of the parsing code cannot handle null characters, so if a malformed server sends some, treat is as a newline
    (C == '\0');
}

void FtpListParser::AddData(const char * Data, qsizetype Size)
{
  FBuffer.append(Data, static_cast<size_t>(Size));

  // Just in case the previous buffer was terminated between CR and LF.
  size_t Skip = 0;
  while ((Skip < FBuffer.size()) && IsNewLineChar(FBuffer[Skip]))
  {
    Skip++;
  }
  FBuffer.erase(0, Skip);

  bool Found;
  size_t Pos = 0;
  size_t FirstLineEnd = 0;
  bool HaveFirstLineEnd = false;
  int Count = 0;
  bool Restart = true;

  // Indexes are 0-based here (1-based RawByteString in the original)
  do
  {
    if (Restart)
    {
      Pos = 0;
      HaveFirstLineEnd = false;
      Count = 0;
      Restart = false;
    }

    while ((Pos < FBuffer.size()) && !IsNewLineChar(FBuffer[Pos]))
    {
      Pos++;
    }
    Found = (Pos < FBuffer.size());
    if (Found)
    {
      Count++;
      std::string Record = FBuffer.substr(0, Pos);
      while ((Pos < FBuffer.size()) && IsNewLineChar(FBuffer[Pos]))
      {
        Pos++;
      }
      if (!HaveFirstLineEnd)
      {
        FirstLineEnd = Pos;
        HaveFirstLineEnd = true;
      }
      FtpListEntry DirEntry;
      std::string Line = Record;
      for (char & C : Line)
      {
        if (IsNewLineChar(C))
        {
          C = ' ';
        }
      }
      if (parseLine(Line.c_str(), Line.size(), DirEntry))
      {
        if ((DirEntry.Name != QStringLiteral(".")) && (DirEntry.Name != QStringLiteral("..")))
        {
          AddLine(DirEntry);
        }
        FBuffer.erase(0, Pos);
        Restart = true;
      }
      else
      {
        if (Count == 2)
        {
          std::string FirstLine = FBuffer.substr(0, FirstLineEnd);
          while (!FirstLine.empty() && (static_cast<unsigned char>(FirstLine.back()) <= ' '))
          {
            FirstLine.pop_back();
          }
          FUnparsed.append(getStr(FirstLine.data(), FirstLine.size()));
          FBuffer.erase(0, FirstLineEnd);
          Restart = true;
        }
      }
    }
  }
  while (Found);
}

void FtpListParser::AddLine(FtpListEntry & direntry)
{
  // (time zone offset is applied by the file system, not here)
  if ((FServerType & stVMS) && (!FVmsAllRevisions || direntry.Dir))
  {
    // Remove version information, only keep the latest file
    qsizetype pos = direntry.Name.lastIndexOf(QLatin1Char(';'));
    if ((pos <= 0) || (pos >= (direntry.Name.size() - 1)))
      return;
    int version = direntry.Name.mid(pos + 1).toInt();
    direntry.Name = direntry.Name.left(pos);

    for (qsizetype Index = 0; Index < FEntries.size(); Index++)
    {
      if (direntry.Name == FEntries[Index].Name)
      {
        if (version > FVersions[Index])
        {
          FEntries[Index] = direntry;
          FVersions[Index] = version;
        }
        return;
      }
    }
    FEntries.append(direntry);
    FVersions.append(version);
  }
  else
  {
    FEntries.append(direntry);
    FVersions.append(0);
  }
}

bool FtpListParser::IsNumeric(const char * str, size_t len) const
{
  if (!str)
    return false;
  if (!*str)
    return false;
  const char * p = str;
  while (*p)
  {
    if (static_cast<size_t>(p - str) >= len)
      return true;

    if (*p < '0' || *p > '9')
    {
      return false;
    }
    p++;
  }
  return true;
}

bool FtpListParser::ParseShortDate(const char * str, size_t len, FtpListDate & date) const
{
  if (!str)
    return false;

  if (len <= 0)
    return false;

  size_t i = 0;

  //Extract the date
  bool bGotYear = false;
  bool bGotMonth = false;
  bool bGotDay = false;
  int value = 0;
  bool numeric = true;
  while (str[i] != '-' && str[i] != '.' && str[i] != '/')
  {
    if (!str[i])
      return false;

    // Left half of token not numeric, check if it's a month name
    if (str[i] < '0' || str[i] > '9')
      numeric = false;
    else
    {
      value *= 10;
      value += str[i] - '0';
    }
    i++;
    if (i == len)
      return false;
  }
  if (!i)
    return false;

  if (!numeric)
  {
    int Month = FindMonth(str, i);
    if (Month == 0)
      return false;

    date.Month = Month;
    bGotMonth = true;
  }
  else if (i == 4)
  { //Seems to be yyyy-mm-dd
    if (value < 1900)
      return false;
    date.Year = value;
    bGotYear = true;
  }
  else if (i <= 2)
  {
    if (str[i] == '.')
    {
      // Maybe dd.mm.yyyy
      if (!value || value > 31)
        return false;
      date.Day = value;
      bGotDay = true;
    }
    else
    {
      // Seems to be mm-dd-yyyy or mm/dd/yyyy (stupid format, though)
      if (!value)
        return false;
      else if (value > 12) // sigh, guess dd/mm/yyyy instead
      {
        date.Day = value;
        bGotDay = true;
      }
      else
      {
        date.Month = value;
        bGotMonth = true;
      }
    }
  }
  else
    return false;
  (void)bGotMonth;

  //Extract the second date field
  const char * p = str + i + 1;
  len -= i + 1;
  i = 0;
  value = 0;

  if (i >= len)
    return false;

  while (p[i] != '-' && p[i] != '.' && p[i] != '/')
  {
    value *= 10;
    value += p[i] - '0';
    i++;

    if (i >= len)
      return false;
  }
  if (bGotYear || bGotDay)
  {
    // Month field in yyyy-mm-dd or dd-mm-yyyy
    if (!value || value > 12)
      return false;
    date.Month = value;
  }
  else
  {
    // Day field in mm-dd-yyyy
    if (!value || value > 31)
      return false;
    date.Day = value;
  }

  //Extract the last date field
  p += i + 1;
  len -= i + 1;
  i = 0;
  value = 0;

  if (i >= len)
    return false;
  while (p[i] != '-' && p[i] != '.' && p[i] != '/')
  {
    value *= 10;
    value += p[i] - '0';
    i++;
    if (i >= len)
      break;
  }

  if (bGotYear)
  {
    // Day field in yyyy-mm-dd
    if (!value || value > 31)
      return false;
    date.Day = value;
  }
  else
  {
    //Year in dd.mm.yyyy or mm-dd-yyyy
    date.Year = value;
    if (date.Year < 50)
      date.Year += 2000;
    else if (date.Year < 1000)
      date.Year += 1900;
  }

  date.HasDate = true;
  date.HasYear = true;
  return true;
}

bool FtpListParser::parseAsVMS(const char * line, const size_t linelen, FtpListEntry & direntry)
{
  size_t tokenlen = 0;
  size_t pos = 0;

  FtpListEntry dir;

  dir.Unsure = false;

  const char * str = GetNextToken(line, linelen, tokenlen, pos, 0);
  if (!str)
    return false;

  if (!strnchr(str, tokenlen, ';'))
    return false;

  dir.Size = -2;
  const char * separator = strnchr(str, tokenlen, ';');
  if (!separator)
    return false;

  dir.Dir = false;

  if ((separator - str) > 4)
    if (*(separator - 4) == '.')
      if (*(separator - 3) == 'D')
        if (*(separator - 2) == 'I')
          if (*(separator - 1) == 'R')
          {
            dir.Dir = true;
          }
  if (dir.Dir)
  {
    // name without ".DIR" (version kept)
    std::string Buf(str, static_cast<size_t>(separator - str) - 4);
    Buf.append(separator, tokenlen - static_cast<size_t>(separator - str));
    dir.Name = getStr(Buf.data(), Buf.size());
  }
  else
    dir.Name = getStr(str, tokenlen);

  // This field is either the size or a username (???) enclosed in [].
  str = GetNextToken(line, linelen, tokenlen, pos, 0);
  if (!str)
    return false;

  bool gotSize = false;
  const char * p = strnchr(str, tokenlen, '/');
  if (!p && IsNumeric(str, tokenlen))
  {
    gotSize = true;
    dir.Size = strntoi64(str, tokenlen) * 512;
  }
  else if (p && p > str && IsNumeric(str, static_cast<size_t>(p - str)))
  {
    gotSize = true;
    dir.Size = strntoi64(str, static_cast<size_t>(p - str)) * 512;
  }
  else
  {
    if (tokenlen < 3 || str[0] != '[' || str[tokenlen - 1] != ']')
      return false;
    dir.OwnerGroup = getStr(str + 1, tokenlen - 2);
  }

  if (!gotSize)
  {
    //Size
    str = GetNextToken(line, linelen, tokenlen, pos, 0);
    if (!str)
      return false;

    const char * p2 = strnchr(str, tokenlen, '/');
    size_t len;
    if (p2)
      len = static_cast<size_t>(p2 - str);
    else
      len = tokenlen;

    if (!IsNumeric(str, len))
      return false;

    dir.Size = strntoi64(str, len) * 512;
  }

  //Get date
  str = GetNextToken(line, linelen, tokenlen, pos, 0);
  if (!str)
    return false;

  dir.Date.HasDate = true;

  //Day
  p = str;

  while (*p != '-')
    if (static_cast<size_t>(++p - str) == tokenlen)
      return false;

  dir.Date.Day = static_cast<int>(strntoi64(str, static_cast<size_t>(p - str)));
  if (!dir.Date.Day || dir.Date.Day > 31)
    return false;

  p++;
  const char * pMonth = p;
  //Month
  while (*p != '-')
    if (static_cast<size_t>(++p - str) == tokenlen)
      return false;
  if ((p - pMonth) >= 15)
    return false;
  int Month = FindMonth(pMonth, static_cast<size_t>(p - pMonth));
  if (Month == 0)
    return false;
  dir.Date.Month = Month;
  p++;

  dir.Date.Year = static_cast<int>(strntoi64(p, tokenlen - static_cast<size_t>(p - str)));
  dir.Date.HasYear = true;

  //Get time
  str = GetNextToken(line, linelen, tokenlen, pos, 0);

  if (str && (tokenlen >= 2) && strnchr(str + 1, tokenlen - 2, ':'))
  {
    dir.Date.HasTime = true;

    p = str;
    //Hours
    while (*p != ':')
      if (static_cast<size_t>(++p - str) == tokenlen)
        return false;

    dir.Date.Hour = static_cast<int>(strntoi64(str, static_cast<size_t>(p - str)));
    if (dir.Date.Hour < 0 || dir.Date.Hour > 23)
      return false;
    p++;

    const char * pMinute = p;
    //Minutes
    while (*p && *p != ':' && static_cast<size_t>(p - str) < tokenlen)
      p++;

    dir.Date.Minute = static_cast<int>(strntoi64(pMinute, static_cast<size_t>(p - pMinute)));
    if (dir.Date.Minute < 0 || dir.Date.Minute > 59)
      return false;

    str = GetNextToken(line, linelen, tokenlen, pos, 0);
  }
  else
  {
    dir.Date.HasTime = false;
  }

  while (str)
  {
    if (tokenlen > 2 && str[0] == '(' && str[tokenlen - 1] == ')')
    {
      if (!dir.Permissions.isEmpty())
        dir.Permissions += QLatin1Char(' ');
      dir.Permissions += getStr(str + 1, tokenlen - 2);
    }
    else if (tokenlen > 2 && str[0] == '[' && str[tokenlen - 1] == ']')
    {
      if (!dir.OwnerGroup.isEmpty())
        dir.OwnerGroup += QLatin1Char(' ');
      dir.OwnerGroup += getStr(str + 1, tokenlen - 2);
    }
    else
    {
      if (!dir.Permissions.isEmpty())
        dir.Permissions += QLatin1Char(' ');
      dir.Permissions += getStr(str, tokenlen);
    }

    str = GetNextToken(line, linelen, tokenlen, pos, 0);
  }

  direntry = dir;

  return true;
}

bool FtpListParser::parseAsEPLF(const char * line, const size_t, FtpListEntry & direntry)
{
  FtpListEntry dir;
  const char * str = strstr(line, "\t");

  //Check if directory listing is an EPLF one
  if (*line == '+' && str)
  {
    str++;
    if (!*str)
      return false;
    dir.Link = false;
    dir.Unsure = false;
    dir.Date.HasDate = dir.Date.HasTime = false;
    dir.Dir = false;
    dir.Size = -2;
    dir.Name = getStr(str, strlen(str), true);
    QString Permissions;
    const char * fact = line + 1;
    const char * nextfact = fact;
    nextfact = strstr(nextfact, ",");
    while (fact <= (str - 2))
    {
      size_t len;
      if (!nextfact)
        len = static_cast<size_t>(str - fact - 1);
      else
        len = static_cast<size_t>(nextfact - fact);
      if (len == 1 && fact[0] == '/')
        dir.Dir = true;
      else if (*fact == 's')
        dir.Size = strntoi64(fact + 1, len - 1);
      else if (*fact == 'm')
      {
        qint64 rawtime = strntoi64(fact + 1, len - 1);
        TimeTToDate(rawtime, dir.Date);
      }
      else if (len == 5 && *fact == 'u' && *(fact + 1) == 'p')
      {
        // (the original assigned this to the output entry, which is then
        // overwritten, so the permissions were lost)
        Permissions = QString::fromLatin1(fact + 2, 3);
      }
      if (!nextfact || nextfact >= (str - 2))
        break;
      fact = nextfact + 1;
      nextfact = strstr(nextfact + 1, ",");
    }
    dir.Permissions = Permissions;

    direntry = dir;

    return true;
  }

  return false;
}

bool FtpListParser::parseAsMlsd(const char * line, const size_t linelen, FtpListEntry & direntry)
{
  // MLSD format as described here: https://www.rfc-editor.org/info/rfc3659/
  // Parsing is done strict, abort on slightest error.

  size_t pos = 0;
  size_t tokenlen = 0;

  const char * str = GetNextToken(line, linelen, tokenlen, pos, 0);
  if (!str)
  {
    return false;
  }
  QString facts = QString::fromLatin1(str, static_cast<qsizetype>(tokenlen));
  if (facts.isEmpty())
  {
    return false;
  }
  direntry.Name.clear();
  direntry.Unsure = false;
  direntry.Dir = false;
  direntry.Link = false;
  direntry.OwnerGroup.clear();
  direntry.Owner.clear();
  direntry.Group.clear();
  direntry.Permissions.clear();

  QString owner, group, uid, gid, ownername, groupname;

  while (!facts.isEmpty())
  {
    qsizetype delim = facts.indexOf(QLatin1Char(';'));
    if (delim < 3)
    {
      if (delim != -1)
      {
        return false;
      }
      else
        delim = facts.size();
    }

    qsizetype eq = facts.indexOf(QLatin1Char('='));
    if (eq < 1 || eq > delim)
    {
      return false;
    }

    QString factname = facts.left(eq).toLower();
    QString value = facts.mid(eq + 1, delim - eq - 1);
    if (factname == QStringLiteral("type"))
    {
      if (!value.compare(QStringLiteral("dir"), Qt::CaseInsensitive))
      {
        direntry.Dir = true;
      }
      // This is syntax used by proftpd by default
      else if (!value.left(15).compare(QStringLiteral("OS.unix=symlink"), Qt::CaseInsensitive))
      {
        direntry.Dir = true;
        direntry.Link = true;
        if ((value.size() > 16) && (value[15] == QLatin1Char(':')))
          direntry.LinkTarget = value.mid(16);
      }
      // This is syntax shown in RFC 3659 section 7.7.4 "A More Complex Example"
      else if (!value.left(13).compare(QStringLiteral("OS.unix=slink"), Qt::CaseInsensitive))
      {
        direntry.Dir = true;
        direntry.Link = true;
        if ((value.size() > 14) && (value[13] == QLatin1Char(':')))
          direntry.LinkTarget = value.mid(14);
      }
      // For MLSD, these will be skipped in AddData.
      else if (!value.compare(QStringLiteral("cdir"), Qt::CaseInsensitive))
      {
        // ProFTPD up to 1.3.6rc1 and 1.3.5a incorrectly uses "cdir" for the current working directory.
        // So at least in MLST, where this would be the only entry, we treat it like "dir".
        if (!FMlst)
        {
          direntry.Name = QStringLiteral(".");
        }
        direntry.Dir = true;
      }
      else if (!value.compare(QStringLiteral("pdir"), Qt::CaseInsensitive))
      {
        direntry.Name = QStringLiteral("..");
        direntry.Dir = true;
      }
    }
    else if (factname == QStringLiteral("size"))
    {
      direntry.Size = 0;

      for (qsizetype i = 0; i < value.size(); ++i)
      {
        if (value[i] < QLatin1Char('0') || value[i] > QLatin1Char('9'))
        {
          return false;
        }
        direntry.Size *= 10;
        direntry.Size += value[i].unicode() - '0';
      }
    }
    else if (factname == QStringLiteral("modify") ||
      (!direntry.Date.HasDate && factname == QStringLiteral("create")))
    {
      if (!parseMlsdDateTime(value, direntry.Date))
      {
        return false;
      }
    }
    else if (factname == QStringLiteral("perm"))
    {
      // there's no way we can convert Perm fact to unix-style permissions,
      // so we at least present Perm as-is to a user
      direntry.HumanPermissions = value;
    }
    else if (factname == QStringLiteral("unix.mode"))
    {
      direntry.Permissions = value;
    }
    else if (factname == QStringLiteral("unix.owner") || factname == QStringLiteral("unix.user"))
    {
      owner = value;
    }
    else if (factname == QStringLiteral("unix.group"))
    {
      group = value;
    }
    else if (factname == QStringLiteral("unix.uid"))
    {
      uid = value;
    }
    else if (factname == QStringLiteral("unix.gid"))
    {
      gid = value;
    }
    else if (factname == QStringLiteral("unix.ownername"))
    {
      ownername = value;
    }
    else if (factname == QStringLiteral("unix.groupname"))
    {
      groupname = value;
    }

    facts = facts.mid(delim + 1);
  }

  // The order of the facts is undefined
  if (!ownername.isEmpty())
    direntry.Owner = ownername;
  else if (!owner.isEmpty())
    direntry.Owner = owner;
  else if (!uid.isEmpty())
    direntry.Owner = uid;

  if (!groupname.isEmpty())
    direntry.Group = groupname;
  else if (!group.isEmpty())
    direntry.Group = group;
  else if (!gid.isEmpty())
    direntry.Group = gid;

  if (line[pos] != ' ')
  {
    return false;
  }
  if (direntry.Name.isEmpty())
  {
    pos++;
    QString fileName = getStr(line + pos, linelen - pos, true);
    if (FMlst)
    {
      // do not try to detect path type, assume a standard *nix syntax + do not trim
      QString Name = fileName;
      while ((Name.size() > 1) && Name.endsWith(QLatin1Char('/')))
      {
        Name.chop(1);
      }
      qsizetype Slash = Name.lastIndexOf(QLatin1Char('/'));
      direntry.Name = (Slash >= 0) ? Name.mid(Slash + 1) : Name;
      if (direntry.Name.isEmpty())
      {
        direntry.Name = fileName;
      }
    }
    else
    {
      direntry.Name = fileName;
    }
  }
  return true;
}

bool FtpListParser::parseMlsdDateTime(const QString & value, FtpListDate & date) const
{
  if (value.isEmpty())
  {
    return false;
  }

  bool result = false;
  int Year, Month, Day, Hours, Minutes, Seconds;
  Year = Month = Day = Hours = Minutes = Seconds = 0;
  // Time can include a fraction after a dot, this will ignore the fraction part.
  QByteArray V = value.toLatin1();
  if (sscanf(V.constData(), "%4d%2d%2d%2d%2d%2d", &Year, &Month, &Day, &Hours, &Minutes, &Seconds) == 6)
  {
    date.HasDate = true;
    date.HasTime = true;
    date.HasSeconds = true;
    result = true;
  }
  else if (sscanf(V.constData(), "%4d%2d%2d", &Year, &Month, &Day) == 3)
  {
    date.HasDate = true;
    date.HasTime = false;
    result = true;
  }
  if (result)
  {
    date.Year = Year;
    date.HasYear = true;
    date.Month = Month;
    date.Day = Day;
    date.Hour = Hours;
    date.Minute = Minutes;
    date.Second = Seconds;
    date.Utc = true;
  }
  return result;
}

void FtpListParser::GuessYearIfUnknown(FtpListDate & Date) const
{
  // Problem: Some servers use times only for files newer than 6 months,
  // others use one year as limit. IIS shows time for files from the current year (jan-dec).
  // So there is no support for files with time
  // dated in the near future. Under normal conditions there should not be such files.
  if (!Date.Year)
  {
    int curday = FCurrentDate.day();
    int curmonth = FCurrentDate.month();
    int curyear = FCurrentDate.year();
    int now = curmonth * 31 + curday;
    int file = Date.Month * 31 + Date.Day;
    if ((now + 1) >= file)
    {
      Date.Year = curyear;
    }
    else
    {
      Date.Year = curyear - 1;
    }
    // year is guessed, not setting hasyear
  }
}

bool FtpListParser::parseAsUnix(const char * line, const size_t linelen, FtpListEntry & direntry)
{
  size_t pos = 0;
  size_t tokenlen = 0;

  const char * str = GetNextToken(line, linelen, tokenlen, pos, 0);
  if (!str)
  {
    return false;
  }
  //Check the first token
  if (str[0] != 'b' &&
    str[0] != 'c' &&
    str[0] != 'd' &&
    str[0] != 'l' &&
    str[0] != 'p' &&
    str[0] != 's' &&
    str[0] != '-')
  {
    return false;
  }

  //First check if it is a netware server
  bool bNetWare = false;
  direntry.Permissions = getStr(str, tokenlen);
  if (tokenlen == 1)
  {
    //Yes, it's most likely a netware server
    //Now get the full permission string
    bNetWare = true;
    str = GetNextToken(line, linelen, tokenlen, pos, 0);
    if (!str)
    {
      return false;
    }
    direntry.Permissions += QLatin1Char(' ');
    direntry.Permissions += getStr(str, tokenlen);
  }

  //Set directory and link flags
  //Always assume links point directories
  //GUI frontend should try to figure out
  //to where the link really points
  QChar Type = direntry.Permissions[0];
  direntry.Dir = (Type == QLatin1Char('d') || Type == QLatin1Char('l'));
  direntry.Link = (Type == QLatin1Char('l'));

  bool bNetPresenz = false;
  if (!bNetWare) //On non-netware servers, expect at least two unused tokens
  {
    bool groupid = false;
    // Unused param
    str = GetNextToken(line, linelen, tokenlen, pos, 0);
    if (!str)
    {
      return false;
    }
    if (tokenlen == 6 && !strncmp(str, "folder", tokenlen) && direntry.Dir)
      bNetPresenz = true;  //Assume NetPresenz server
                //However, it's possible that we mark a non-NetPresenz
                //server if the fileowner is "folder"
    else if (!IsNumeric(str, tokenlen))
    {
      // Check for Connect:Enterprise server
      QString Right3 = direntry.Permissions.right(3);
      if (direntry.Permissions.size() > 3 &&
        (Right3 == QStringLiteral("FTP") ||
         Right3 == QStringLiteral("FTS") ||
         Right3 == QStringLiteral("TCP") ||
         Right3 == QStringLiteral("SSH")))
        groupid = true;

      direntry.OwnerGroup += getStr(str, tokenlen);
    }
    else
      groupid = true;

    if (!bNetPresenz && groupid)
    {
      //Unused param
      str = GetNextToken(line, linelen, tokenlen, pos, 0);
      if (!str)
      {
        return false;
      }

      if (!direntry.OwnerGroup.isEmpty())
        direntry.OwnerGroup += QLatin1Char(' ');
      direntry.OwnerGroup += getStr(str, tokenlen);
    }
  }

  //Skip param, may be used for size
  size_t skippedlen = 0;
  const char * skipped = GetNextToken(line, linelen, skippedlen, pos, 0);
  if (!skipped)
  {
    return false;
  }

  str = GetNextToken(line, linelen, tokenlen, pos, 0);
  if (!str)
  {
    return false;
  }

  // Keep parsing the information until we get a numerical string,
  // because some broken servers may send 3 tags: domain/network, group and user
  const char * prevstr = nullptr;
  size_t prevstrlen = 0;

  qint64 tmp = 0;
  while (str && !ParseSize(str, tokenlen, tmp) && !IsNumeric(skipped, skippedlen))
  {
    //Maybe the server has left no space between the group and the size
    //because of stupid alignment
    if (FindMonth(str, tokenlen) != 0)
    {
      bool bRightNumeric = true;
      if (skipped[skippedlen - 1] < '0' || skipped[skippedlen - 1] > '9')
        bRightNumeric = false;

      if (bRightNumeric)
        break;

      bRightNumeric = true;
      if (prevstr && prevstrlen)
      {
        if (prevstr[prevstrlen - 1] < '0' || prevstr[prevstrlen - 1] > '9')
          bRightNumeric = false;

        if (bRightNumeric)
        {
          if (!direntry.OwnerGroup.isEmpty())
            direntry.OwnerGroup += QLatin1Char(' ');
          direntry.OwnerGroup += getStr(str, tokenlen);
          skipped = prevstr;
          skippedlen = prevstrlen;
          break;
        }
      }
    }
    if (prevstr)
    {
      if (!direntry.OwnerGroup.isEmpty())
        direntry.OwnerGroup += QLatin1Char(' ');

      direntry.OwnerGroup += getStr(prevstr, prevstrlen);
    }
    prevstr = str;
    prevstrlen = tokenlen;
    str = GetNextToken(line, linelen, tokenlen, pos, 0);
  }

  if (!str)
  {
    return false;
  }

  const char * size = str;
  size_t sizelen = tokenlen;
  if (!ParseSize(str, tokenlen, direntry.Size))
  {
    //Maybe we've skipped too much tokens
    if (!ParseSize(skipped, skippedlen, direntry.Size))
    {
      //Maybe the server has left no space between the group and the size
      //because of stupid alignment
      bool bRightNumeric = true;
      const char * p;
      for (p = (str + tokenlen - 1); p > str; p--)
      {
        if (*p < '0' || *p > '9')
        {
          if (p == (str + tokenlen - 1))
            bRightNumeric = false;
          break;
        }
      }
      if (bRightNumeric && p > str)
      {
        size = p + 1;
        sizelen = static_cast<size_t>(p - str);
        direntry.OwnerGroup += QLatin1Char(' ');

        if (!direntry.OwnerGroup.isEmpty())
          direntry.OwnerGroup += QLatin1Char(' ');

        direntry.OwnerGroup += getStr(str, static_cast<size_t>(p - str));
      }
      else
      {
        for (p = (skipped + skippedlen - 1); p > skipped; p--)
        {
          if (*p < '0' || *p > '9')
          {
            if (p == (skipped + skippedlen - 1))
            {
              return false;
            }
            break;
          }
        }
        size = p + 1;
        sizelen = skippedlen - static_cast<size_t>(size - skipped);

        if (!direntry.OwnerGroup.isEmpty())
          direntry.OwnerGroup += QLatin1Char(' ');

        direntry.OwnerGroup += getStr(skipped, skippedlen - sizelen);
      }
      direntry.Size = strntoi64(size, sizelen);
    }
    else
    {
      //We should've not skipped the last token
      //This also fixes the problem with the NetPresenz detection
      if (bNetPresenz && direntry.Dir && !direntry.OwnerGroup.isEmpty())
      {
        direntry.OwnerGroup = QStringLiteral("folder ") + direntry.OwnerGroup;
      }
    }
  }
  else
  {
    if (!direntry.OwnerGroup.isEmpty())
      direntry.OwnerGroup += QLatin1Char(' ');
    direntry.OwnerGroup += getStr(skipped, skippedlen);
    if (prevstr)
    {
      direntry.OwnerGroup += QLatin1Char(' ');
      direntry.OwnerGroup += getStr(prevstr, prevstrlen);
    }
    str = nullptr;
  }

  //Month
  if (!str)
    str = GetNextToken(line, linelen, tokenlen, pos, 0);

  if (!str)
  {
    return false;
  }

  const char * smonth = str;
  size_t smonthlen = tokenlen;
  direntry.Date.Year = 0;

  //Day
  const char * sday = nullptr;
  size_t sdaylen = 0;

  // Some VShell server send both the year and the time, try to detect them
  bool bCouldBeVShell = false;

  auto SwapMonthDay = [&]()
  {
    const char * t = smonth;
    smonth = sday;
    sday = t;
    size_t tl = smonthlen;
    smonthlen = sdaylen;
    sdaylen = tl;
  };

  //Some servers use the following date formats:
  // 26-09 2002, 2002-10-14, 01-jun-99
  const char * p = strnchr(smonth, smonthlen, '-');
  if (p)
  {
    size_t plen = smonthlen - static_cast<size_t>(p - smonth);
    const char * pos2 = strnchr(p + 1, plen - 1, '-');
    if (!pos2) //26-09 2002
    {
      sday = p + 1;
      sdaylen = plen - 1;
      smonthlen = static_cast<size_t>(p - smonth);
    }
    else if (p - smonth == 4) //2002-10-14
    {
      direntry.Date.Year = static_cast<int>(strntoi64(smonth, static_cast<size_t>(p - smonth)));
      direntry.Date.HasYear = true;
      sday = pos2 + 1;
      sdaylen = smonthlen - static_cast<size_t>(pos2 - smonth) - 1;
      smonthlen = static_cast<size_t>(pos2 - smonth) - static_cast<size_t>(p - smonth) - 1;
      smonth = p + 1;
      /* Try to detect difference between yyyy/dd/mm and yyyy/mm/dd
       * Unfortunately we have to guess which one is the right if
       * the month is < 12
       */
      if (strntoi64(smonth, smonthlen) > 12)
      {
        SwapMonthDay();
      }
    }
    else if (p - smonth) //14-10-2002 or 01-jun-99
    {
      direntry.Date.Year = static_cast<int>(strntoi64(pos2 + 1, tokenlen - static_cast<size_t>(pos2 - smonth) - 1));
      direntry.Date.HasYear = true;
      sday = smonth;
      sdaylen = static_cast<size_t>(p - smonth);
      smonthlen = static_cast<size_t>(pos2 - smonth) - static_cast<size_t>(p - smonth) - 1;
      smonth = p + 1;
      if (strntoi64(smonth, smonthlen) > 12)
      {
        SwapMonthDay();
      }
    }
    else
    {
      return false;
    }
  }
  /* Some servers use the following date formats:
   * yyyy/dd/mm, yyyy/mm/dd, dd/mm/yyyy, mm/dd/yyyy
   * try to detect them.
   */
  else if (strnchr(smonth, smonthlen, '/'))
  {
    const char * p2 = strnchr(smonth, smonthlen, '/');
    size_t plen = smonthlen - static_cast<size_t>(p2 - smonth);
    const char * pos2 = strnchr(p2 + 1, plen - 1, '/');
    if (!pos2) //Assume 26/09 2002
    {
      sday = p2 + 1;
      sdaylen = plen - 1;
      smonthlen = static_cast<size_t>(p2 - smonth);
    }
    else if (p2 - smonth == 4)
    {
      direntry.Date.Year = static_cast<int>(strntoi64(smonth, static_cast<size_t>(p2 - smonth)));
      direntry.Date.HasYear = true;
      sday = pos2 + 1;
      sdaylen = smonthlen - static_cast<size_t>(pos2 - smonth) - 1;
      smonthlen = static_cast<size_t>(pos2 - smonth) - static_cast<size_t>(p2 - smonth) - 1;
      smonth = p2 + 1;
      if (strntoi64(smonth, smonthlen) > 12)
      {
        SwapMonthDay();
      }
    }
    else if (p2 - smonth == 2)
    {
      direntry.Date.Year = static_cast<int>(strntoi64(pos2 + 1, tokenlen - static_cast<size_t>(pos2 - smonth) - 1));
      direntry.Date.HasYear = true;
      sday = smonth;
      sdaylen = static_cast<size_t>(p2 - smonth);
      smonthlen = static_cast<size_t>(pos2 - smonth) - static_cast<size_t>(p2 - smonth) - 1;
      smonth = p2 + 1;
      if (strntoi64(smonth, smonthlen) > 12)
      {
        SwapMonthDay();
      }
    }
    else
    {
      return false;
    }
  }
  else
  {
    str = GetNextToken(line, linelen, tokenlen, pos, 0);
    if (!str)
    {
      return false;
    }

    sday = str;
    sdaylen = tokenlen;

    if (sdaylen && sday[sdaylen - 1] == ',')
    {
      sdaylen--;
      bCouldBeVShell = true;
    }

    //Trim trailing characters
    while (sdaylen && (sday[sdaylen - 1] == '.' || sday[sdaylen - 1] == ','))
    {
      bCouldBeVShell = false;
      sdaylen--;
    }

    size_t i;
    for (i = 0; i < sdaylen; i++)
      if (sday[i] < '0' || sday[i] > '9')
        break;
    if (i && i < sdaylen)
    {
      if (static_cast<unsigned char>(sday[i]) > 127)
        sdaylen = i;
    }

    if (!sdaylen)
    {
      return false;
    }
  }

  if (!strntoi64(sday, sdaylen)) //Day field invalid
  { //Maybe the server is sending a directory listing with localized date format.
    //Try to fix this really bad behavior
    bCouldBeVShell = false;
    SwapMonthDay();
  }

  //Time/Year
  str = GetNextToken(line, linelen, tokenlen, pos, 0);
  if (!str)
  {
    return false;
  }

  //Trim trailing characters
  while (smonthlen && (smonth[smonthlen - 1] == '.' || smonth[smonthlen - 1] == ','))
    smonthlen--;

  if (!smonthlen)
  {
    return false;
  }

  bool gotYear = false;
  int year = static_cast<int>(strntoi64(smonth, smonthlen));
  if (year > 1000)
  {
    gotYear = true;
    direntry.Date.Year = year;
    direntry.Date.HasYear = true;
  }
  else
  {
    //Try if we can recognize the month name
    int Month = FindMonth(smonth, smonthlen);
    if (Month == 0)
    {
      size_t i;
      for (i = 0; i < smonthlen; i++)
        if (smonth[i] < '0' || smonth[i] > '9')
          break;
      if (!i || i == smonthlen)
      {
        return false;
      }
      if (static_cast<unsigned char>(smonth[i]) < 128)
      {
        return false;
      }

      smonthlen = i;
      direntry.Date.Month = static_cast<int>(strntoi64(smonth, smonthlen));
      if (!direntry.Date.Month || direntry.Date.Month > 12)
      {
        return false;
      }
    }
    else
      direntry.Date.Month = Month;
  }

  if (!gotYear)
  {
    direntry.Date.Day = static_cast<int>(strntoi64(sday, sdaylen));
    if (direntry.Date.Day < 1 || direntry.Date.Day > 31)
    {
      return false;
    }
  }
  else
  {
    direntry.Date.Month = static_cast<int>(strntoi64(sday, sdaylen));
    if (direntry.Date.Month < 1 || direntry.Date.Month > 12)
    {
      return false;
    }
  }

  const char * stimeyear = str;
  size_t stimeyearlen = tokenlen;

  //Parse the time/year token
  const char * strpos = strnchr(stimeyear, stimeyearlen, ':');
  if (!strpos)
    strpos = strnchr(stimeyear, stimeyearlen, '.');
  if (!strpos)
    strpos = strnchr(stimeyear, stimeyearlen, '-');
  if (strpos)
  {
    //stimeyear has delimiter, so it's a time
    direntry.Date.Hour = static_cast<int>(strntoi64(stimeyear, static_cast<size_t>(strpos - stimeyear)));
    size_t stimeyearrem = stimeyearlen - static_cast<size_t>(strpos - stimeyear) - 1;
    const char * strpos2 = strnchr(strpos + 1, stimeyearrem, ':');
    if (strpos2 == nullptr)
    {
      direntry.Date.Minute = static_cast<int>(strntoi64(strpos + 1, stimeyearrem));
    }
    else
    {
      direntry.Date.Minute = static_cast<int>(strntoi64(strpos + 1, static_cast<size_t>(strpos2 - strpos - 1)));
      direntry.Date.Second = static_cast<int>(strntoi64(strpos2 + 1, stimeyearlen - static_cast<size_t>(strpos2 - stimeyear) - 1));
      direntry.Date.HasSeconds = true;
    }
    direntry.Date.HasTime = true;

    GuessYearIfUnknown(direntry.Date);
    bCouldBeVShell = false;
  }
  else
  {
    if (gotYear)
    {
      direntry.Date.Day = static_cast<int>(strntoi64(stimeyear, stimeyearlen));
      if (direntry.Date.Day < 1 || direntry.Date.Day > 31)
      {
        return false;
      }
    }
    else if (!direntry.Date.Year)
    {
      //No delimiters -> year

      direntry.Date.HasTime = false;
      direntry.Date.Year = static_cast<int>(strntoi64(stimeyear, stimeyearlen));
      direntry.Date.HasYear = true;
    }
    else
    {
      // File has no time token and short date format
      pos -= stimeyearlen;
    }
  }

  if (!direntry.Date.Year) //Year 0? Really ancient file, this is invalid!
  {
    return false;
  }

  // Check if server could still be one of the newer VShell servers
  if (bCouldBeVShell)
  {
    size_t oldpos = pos;

    //Get time
    str = GetNextToken(line, linelen, tokenlen, pos, 0);
    if (!str)
    {
      return false;
    }

    const char * p2 = strnchr(str, tokenlen, ':');
    if (pos && p2 && (p2 - str) == 2 && IsNumeric(str, 2) && IsNumeric(str + 3, 2))
    {
      //stimeyear has delimiter, so it's a time
      int hour = static_cast<int>(strntoi64(str, 2));
      int minute = static_cast<int>(strntoi64(str + 3, 2));

      if (hour >= 0 && hour < 24 && minute >= 0 && minute < 60)
      {
        direntry.Date.Hour = hour;
        direntry.Date.Minute = minute;
        direntry.Date.HasTime = true;
      }
      else
        pos = oldpos;
    }
    else
      pos = oldpos;
  }

  //Get filename
  str = GetNextToken(line, linelen, tokenlen, pos, 1);
  if (!str)
  {
    return false;
  }

  //Trim link data from filename
  if (direntry.Link)
  {
    const char * arrow = strnstr(str, tokenlen, " -> ");
    if (arrow)
    {
      direntry.LinkTarget = getStr(arrow + 4, tokenlen - static_cast<size_t>(arrow - str) - 4);
      tokenlen = static_cast<size_t>(arrow - str);
    }

    if (!tokenlen)
    {
      return false;
    }
  }

  //Trim indicators, some server add those to mark special files
  if (str[tokenlen - 1] == '*' ||
    str[tokenlen - 1] == '/' ||
    // '=' is not trimmed, it would cause problems on certain servers
    str[tokenlen - 1] == '|')
    tokenlen--;

  direntry.Name = getStr(str, tokenlen, true);

  direntry.Unsure = false;
  direntry.Date.HasDate = true;

  return true;
}

bool FtpListParser::parseAsDos(const char * line, const size_t linelen, FtpListEntry & direntry)
{
  size_t pos = 0;
  size_t tokenlen = 0;

  const char * str = GetNextToken(line, linelen, tokenlen, pos, 0);
  if (!str)
    return false;
  //Check the first token
  if (str[0] == 'b' ||
    str[0] == 'c' ||
    str[0] == 'd' ||
    str[0] == 'l' ||
    str[0] == 'p' ||
    str[0] == 's' ||
    str[0] == '-')
    return false;

  if (IsNumeric(str, tokenlen))
    return false;

  //It's a NT server with MSDOS directory format

  if (!ParseShortDate(str, tokenlen, direntry.Date))
    return false;

  //Extract time
  str = GetNextToken(line, linelen, tokenlen, pos, 0);
  if (!str)
    return false;

  if (!parseTime(str, tokenlen, direntry.Date))
    return false;

  str = GetNextToken(line, linelen, tokenlen, pos, 0);
  if (!str)
    return false;
  if (tokenlen == 5 && !memcmp(str, "<DIR>", 5))
  {
    direntry.Dir = true;
  }
  else
  {
    std::string buffer;
    for (size_t i = 0; i < tokenlen; i++)
    {
      if (str[i] != ',')
        buffer += str[i];
    }
    direntry.Dir = false;
    direntry.Size = strntoi64(buffer.c_str(), buffer.size());
  }

  str = GetNextToken(line, linelen, tokenlen, pos, 1);
  if (!str)
    return false;
  direntry.Name = getStr(str, tokenlen, true);

  direntry.Unsure = false;

  return true;
}

void FtpListParser::TimeTToDate(qint64 TimeT, FtpListDate & Date) const
{
  QDateTime DateTime = QDateTime::fromSecsSinceEpoch(TimeT, QTimeZone::UTC);
  Date.Year = DateTime.date().year();
  Date.HasYear = true;
  Date.Month = DateTime.date().month();
  Date.Day = DateTime.date().day();
  Date.Hour = DateTime.time().hour();
  Date.Minute = DateTime.time().minute();
  Date.HasDate = Date.HasTime = true;
  // time_t is UTC (the original left the flag unset, treating it as server time)
  Date.Utc = true;
}

bool FtpListParser::parseAsOther(const char * line, const size_t linelen, FtpListEntry & direntry)
{
  size_t pos = 0;
  size_t tokenlen = 0;

  const char * str = GetNextToken(line, linelen, tokenlen, pos, 0);
  if (!str)
    return false;

  //Check the first token
  if (str[0] == 'b' ||
    str[0] == 'c' ||
    str[0] == 'd' ||
    str[0] == 'l' ||
    str[0] == 'p' ||
    str[0] == 's' ||
    str[0] == '-')
    return false;

  if (!IsNumeric(str, tokenlen))
    return false;

  //Could be numerical Unix style format or VShell format
  //or even an OS2 server

  const char * skipped = str;
  size_t skippedtokenlen = tokenlen;
  str = GetNextToken(line, linelen, tokenlen, pos, 0);
  if (!str)
    return false;

  //If next token is numerical, than it's the numerical Unix style format,
  //else it's the VShell or OS/2 format
  if (IsNumeric(str, tokenlen))
  {
    direntry.Permissions = getStr(skipped, skippedtokenlen);

    if (skippedtokenlen >= 2 && skipped[1] == '4')
      direntry.Dir = true;
    else
      direntry.Dir = false;

    //Unused token
    str = GetNextToken(line, linelen, tokenlen, pos, 0);
    if (!str)
      return false;

    //Size
    str = GetNextToken(line, linelen, tokenlen, pos, 0);
    if (!str)
      return false;

    direntry.Size = strntoi64(str, tokenlen);

    //Date/Time
    str = GetNextToken(line, linelen, tokenlen, pos, 0);
    if (!str)
      return false;

    qint64 secsSince1970 = strntoi64(str, tokenlen);
    TimeTToDate(secsSince1970, direntry.Date);

    str = GetNextToken(line, linelen, tokenlen, pos, 1);
    if (!str)
      return false;
    direntry.Name = getStr(str, tokenlen, true);
  }
  else
  {
    //Get size
    direntry.Size = strntoi64(skipped, skippedtokenlen);

    //Get date, month first
    if (tokenlen >= 15)
      return false;

    int Month = FindMonth(str, tokenlen);
    if (Month == 0)
    {
      direntry.Dir = false;
      while (str)
      {
        //Could be an OS/2 server
        if (tokenlen == 3 && !memcmp(str, "dir", 3))
          direntry.Dir = true;
        else if (tokenlen == 3 && !memcmp(str, "DIR", 3))
          direntry.Dir = true;
        else if (strnchr(str, tokenlen, '-'))
          break;
        str = GetNextToken(line, linelen, tokenlen, pos, 0);
      }
      if (!str)
        return false;

      if (!ParseShortDate(str, tokenlen, direntry.Date))
        return false;

      str = GetNextToken(line, linelen, tokenlen, pos, 0);
      if (!str)
        return false;

      //Parse the time token
      const char * strpos = strnchr(str, tokenlen, ':');
      if (!strpos)
        return false;
      //stimeyear has delimiter, so it's a time
      direntry.Date.Hour = static_cast<int>(strntoi64(str, static_cast<size_t>(strpos - str)));
      direntry.Date.Minute = static_cast<int>(strntoi64(strpos + 1, tokenlen - static_cast<size_t>(strpos - str) - 1));
      direntry.Date.HasTime = true;

      GuessYearIfUnknown(direntry.Date);

      str = GetNextToken(line, linelen, tokenlen, pos, 1);
      if (!str)
        return false;

      if (tokenlen > 6)
      {
        if (!StrNICmp(str + tokenlen - 5, "<DIR>", 5))
        {
          direntry.Dir = true;
          tokenlen -= 5;
          while (tokenlen && (str[tokenlen - 1] == ' ' || str[tokenlen - 1] == '\t'))
            tokenlen--;
        }
        if (!tokenlen)
          return false;
      }

      direntry.Name = getStr(str, tokenlen, true);
    }
    else
    {
      direntry.Date.Month = Month;

      //Day
      str = GetNextToken(line, linelen, tokenlen, pos, 0);
      if (!str)
        return false;

      if (str[tokenlen - 1] == ',')
        tokenlen--;
      if (!tokenlen)
        return false;
      if (!IsNumeric(str, tokenlen))
        return false;

      direntry.Date.Day = static_cast<int>(strntoi64(str, tokenlen));
      if (direntry.Date.Day < 1 || direntry.Date.Day > 31)
        return false;

      //Year
      str = GetNextToken(line, linelen, tokenlen, pos, 0);
      if (!str)
        return false;

      if (!IsNumeric(str, tokenlen))
        return false;

      direntry.Date.Year = static_cast<int>(strntoi64(str, tokenlen));
      direntry.Date.HasYear = true;
      if (direntry.Date.Year < 50)
        direntry.Date.Year += 2000;
      else if (direntry.Date.Year < 1000)
        direntry.Date.Year += 1900;

      direntry.Date.HasDate = true;

      //Now get the time
      str = GetNextToken(line, linelen, tokenlen, pos, 0);
      const char * p = strnchr(str, tokenlen, ':');
      if (!p)
        return false;

      size_t plen = static_cast<size_t>(p - str);
      if (p == str || !IsNumeric(str, plen) || (plen + 1) >= tokenlen || !IsNumeric(p + 1, tokenlen - plen - 1))
        return false;
      direntry.Date.Hour = static_cast<int>(strntoi64(str, plen));
      direntry.Date.Minute = static_cast<int>(strntoi64(p + 1, tokenlen - plen - 1));

      if (direntry.Date.Hour < 0 || direntry.Date.Hour > 24)
        return false;
      if (direntry.Date.Minute < 0 || direntry.Date.Minute > 59)
        return false;

      direntry.Date.HasTime = true;

      str = GetNextToken(line, linelen, tokenlen, pos, 1);
      if (!str)
        return false;

      if (tokenlen > 1 && (str[tokenlen - 1] == '\\' || str[tokenlen - 1] == '/'))
      {
        direntry.Dir = true;
        tokenlen--;
      }
      else
        direntry.Dir = false;

      if (!tokenlen)
        return false;
      direntry.Name = getStr(str, tokenlen, true);
    }
  }

  direntry.Unsure = false;
  direntry.Date.HasDate = true;

  return true;
}

qint64 FtpListParser::strntoi64(const char * str, size_t len) const
{
  qint64 res = 0;
  const char * p = str;
  while (static_cast<size_t>(p - str) < len)
  {
    if (*p < '0' || *p > '9')
      break;
    res *= 10;
    res += *p++ - '0';
  }
  return res;
}

const char * FtpListParser::GetNextToken(const char * line, const size_t linelen, size_t & len, size_t & pos, int type) const
{
  const char * p = line + pos;
  if (static_cast<size_t>(p - line) >= linelen)
    return nullptr;
  while (static_cast<size_t>(p - line) < linelen && (*p == ' ' || *p == '\t'))
    p++;

  if (static_cast<size_t>(p - line) >= linelen)
    return nullptr;

  const char * res = p;

  if (type)
  {
    pos = linelen;
    len = linelen - static_cast<size_t>(p - line);
  }
  else
  {
    while (static_cast<size_t>(p - line) < linelen && *p && *p != ' ' && *p != '\t')
      p++;

    len = static_cast<size_t>(p - res);
    pos = static_cast<size_t>(p - line);
  }

  return res;
}

const char * FtpListParser::strnchr(const char * str, size_t len, char c) const
{
  if (!str)
    return nullptr;

  const char * p = str;
  while (len > 0)
  {
    if (!*p)
      return nullptr;
    if (*p == c)
      return p;
    p++;
    len--;
  }
  return nullptr;
}

const char * FtpListParser::strnstr(const char * str, size_t len, const char * c) const
{
  if (!str)
    return nullptr;
  if (!c)
    return nullptr;
  size_t clen = strlen(c);

  const char * p = str;
  while (len > 0)
  {
    if (!*p)
      return nullptr;
    if (*p == *c)
    {
      if (clen == 1)
        return p;
      else if (len >= clen)
      {
        if (!memcmp(p + 1, c + 1, clen - 1))
          return p;
      }
      else
        return nullptr;
    }
    p++;
    len--;
  }
  return nullptr;
}

QString FtpListParser::getStr(const char * source, size_t len, bool mayInvalidateUTF8)
{
  // CFtpControlSocket::DecodeString
  QByteArray Bytes(source, static_cast<qsizetype>(len));
  if (FUtf8)
  {
    QStringDecoder Decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    QString Result = Decoder.decode(Bytes);
    if (!Decoder.hasError())
    {
      return Result;
    }
    if (mayInvalidateUTF8)
    {
      // "Server does not send proper UTF-8, falling back to local charset"
      FUtf8 = false;
    }
  }
  return FtpDecodeLegacy(Bytes);
}

bool FtpListParser::parseAsIBM(const char * line, const size_t linelen, FtpListEntry & direntry)
{
  size_t pos = 0;
  size_t tokenlen = 0;

  const char * str = GetNextToken(line, linelen, tokenlen, pos, 0);
  if (!str)
    return false;

  direntry.OwnerGroup = getStr(str, tokenlen);

  str = GetNextToken(line, linelen, tokenlen, pos, 0);
  if (!str)
    return false;

  if (!IsNumeric(str, tokenlen))
    return false;

  direntry.Size = strntoi64(str, tokenlen);

  //Date
  str = GetNextToken(line, linelen, tokenlen, pos, 0);
  if (!str)
    return false;
  if (!ParseShortDate(str, tokenlen, direntry.Date))
    return false;

  //Time
  str = GetNextToken(line, linelen, tokenlen, pos, 0);
  if (!str)
    return false;
  if (!parseTime(str, tokenlen, direntry.Date))
    return false;

  //Unused Token
  str = GetNextToken(line, linelen, tokenlen, pos, 0);
  if (!str)
    return false;

  //Name
  str = GetNextToken(line, linelen, tokenlen, pos, 1);
  if (!str)
    return false;

  if (str[tokenlen - 1] == '/')
  {
    direntry.Dir = true;
    if (!--tokenlen)
      return false;
  }
  else
    direntry.Dir = false;

  direntry.Name = getStr(str, tokenlen, true);

  direntry.Unsure = false;

  return true;
}

bool FtpListParser::parseAsIBMMVS(const char * line, const size_t linelen, FtpListEntry & direntry)
{
  size_t pos = 0;
  size_t tokenlen = 0;

  // volume
  const char * str = GetNextToken(line, linelen, tokenlen, pos, 0);
  if (!str)
    return false;

  if (strncmp(str, "Migrated", tokenlen) != 0)
  {
    // unit
    str = GetNextToken(line, linelen, tokenlen, pos, 0);
    if (!str)
      return false;

    if (strncmp(str, "Tape", tokenlen) != 0)
    {
      //Referred Date
      str = GetNextToken(line, linelen, tokenlen, pos, 0);
      if (!str)
        return false;
      if (!ParseShortDate(str, tokenlen, direntry.Date))
      {
        // Perhaps of the following type:
        // TSO004 3390 VSAM FOO.BAR
        if (tokenlen != 4 || strncmp(str, "VSAM", tokenlen))
          return false;

        str = GetNextToken(line, linelen, tokenlen, pos, 1);
        if (!str)
          return false;

        if (strnchr(str, tokenlen, ' '))
          return false;

        direntry.Name = getStr(str, tokenlen, true);
        direntry.Dir = false;
        return true;
      }

      // ext
      str = GetNextToken(line, linelen, tokenlen, pos, 0);
      if (!str)
        return false;
      if (!IsNumeric(str, tokenlen))
        return false;

      size_t prevLen = tokenlen;

      // used
      str = GetNextToken(line, linelen, tokenlen, pos, 0);
      if (!str)
        return false;
      if (IsNumeric(str, tokenlen))
      {
        // recfm
        str = GetNextToken(line, linelen, tokenlen, pos, 0);
        if (!str)
          return false;

        if (IsNumeric(str, tokenlen))
          return false;
      }
      else
      {
        if (prevLen < 6)
          return false;
      }

      // lrecl
      str = GetNextToken(line, linelen, tokenlen, pos, 0);
      if (!str)
        return false;
      if (!IsNumeric(str, tokenlen))
        return false;

      // blksize
      str = GetNextToken(line, linelen, tokenlen, pos, 0);
      if (!str)
        return false;
      if (!IsNumeric(str, tokenlen))
        return false;

      // dsorg
      str = GetNextToken(line, linelen, tokenlen, pos, 0);
      if (!str)
        return false;
      if (tokenlen == 2 && !memcmp(str, "PO", 2))
      {
        direntry.Dir = true;
      }
      else
      {
        direntry.Dir = false;
        direntry.Size = 100;
      }
    }
  }

  // name of dataset or sequential name
  str = GetNextToken(line, linelen, tokenlen, pos, 0);
  if (!str)
    return false;
  direntry.Name = getStr(str, tokenlen, true);

  direntry.Unsure = false;

  return true;
}

bool FtpListParser::parseAsIBMMVSPDS(const char * line, const size_t linelen, FtpListEntry & direntry)
{
  size_t pos = 0;
  size_t tokenlen = 0;

  // pds member name
  const char * str = GetNextToken(line, linelen, tokenlen, pos, 0);
  if (!str)
    return false;
  direntry.Name = getStr(str, tokenlen);

  // vv.mm
  str = GetNextToken(line, linelen, tokenlen, pos, 0);
  if (!str)
    return false;

  // creation date
  str = GetNextToken(line, linelen, tokenlen, pos, 0);
  if (!str)
    return false;

  // change date
  str = GetNextToken(line, linelen, tokenlen, pos, 0);
  if (!str)
    return false;
  if (!ParseShortDate(str, tokenlen, direntry.Date))
    return false;

  // change time
  str = GetNextToken(line, linelen, tokenlen, pos, 0);
  if (!str)
    return false;
  if (!parseTime(str, tokenlen, direntry.Date))
    return false;

  // size
  str = GetNextToken(line, linelen, tokenlen, pos, 0);
  if (!str)
    return false;
  if (!IsNumeric(str, tokenlen))
    return false;
  direntry.Size = strntoi64(str, tokenlen);

  // init
  str = GetNextToken(line, linelen, tokenlen, pos, 0);
  if (!str)
    return false;
  if (!IsNumeric(str, tokenlen))
    return false;

  // mod
  str = GetNextToken(line, linelen, tokenlen, pos, 0);
  if (!str)
    return false;
  if (!IsNumeric(str, tokenlen))
    return false;

  // id
  str = GetNextToken(line, linelen, tokenlen, pos, 0);
  if (!str)
    return false;

  direntry.Dir = false;
  direntry.Unsure = false;

  return true;
}

bool FtpListParser::parseAsIBMMVSPDS2(const char * line, const size_t linelen, FtpListEntry & direntry)
{
  // Use this one only on MVS servers, as it will cause problems on other servers

  if (!(FServerType & (stMVS | stBS2000)))
    return false;

  size_t pos = 0;
  size_t tokenlen = 0;

  direntry.Unsure = false;
  direntry.Dir = false;
  direntry.Link = false;
  direntry.OwnerGroup.clear();
  direntry.Permissions.clear();
  direntry.Date.HasDate = direntry.Date.HasTime = false;

  // pds member name
  const char * str = GetNextToken(line, linelen, tokenlen, pos, 0);
  if (!str)
    return false;

  if ((FServerType & stBS2000) && IsNumeric(str, tokenlen))
  {
    size_t prevlen = tokenlen;
    const char * prev = str;
    size_t oldpos = pos;

    str = GetNextToken(line, linelen, tokenlen, pos, 0);
    if (!str)
      return false;
    if (str[0] != ':')
    {
      str = prev;
      tokenlen = prevlen;
      pos = oldpos;
    }
  }

  direntry.Name = getStr(str, tokenlen);

  if (FServerType & stBS2000)
  {
    qsizetype colon = direntry.Name.lastIndexOf(QLatin1Char(':'));
    if (colon != -1)
      direntry.Name = direntry.Name.mid(colon + 1);
    if (direntry.Name.startsWith(QLatin1Char('$')))
    {
      qsizetype dot = direntry.Name.indexOf(QLatin1Char('.'));
      if (dot != -1)
        direntry.Name = direntry.Name.mid(dot + 1);
    }
    if (direntry.Name.isEmpty())
      return false;
  }

  // Hexadecimal filesize
  str = GetNextToken(line, linelen, tokenlen, pos, 0);
  if (!str)
    // We can return true since sometimes MVS servers send pure filenames
    return true;

  direntry.Size = 0;
  const char * end = str + tokenlen;
  const char * p = str;
  while (p < end)
  {
    direntry.Size *= 16;
    if (*p >= '0' && *p <= '9')
      direntry.Size += *p - '0';
    else if (*p >= 'A' && *p <= 'F')
      direntry.Size += *p - 'A' + 10;
    else
      return false;
    p++;
  }

  // Unused hexadecimal token
  str = GetNextToken(line, linelen, tokenlen, pos, 0);
  if (!str)
    return false;

  end = str + tokenlen;
  p = str;
  while (p < end)
  {
    if ((*p < '0' || *p > '9') && (*p < 'A' || *p > 'F'))
      return false;
    p++;
  }

  // Unused token
  str = GetNextToken(line, linelen, tokenlen, pos, 0);
  if (!str)
    return false;
  if (!IsNumeric(str, tokenlen))
    return false;

  const char * prevprev = nullptr;
  const char * prev = nullptr;
  size_t prevprevlen = 0;
  size_t prevlen = 0;

  str = GetNextToken(line, linelen, tokenlen, pos, 0);
  if (!str)
    return false;
  while (str)
  {
    if (prevprev)
    {
      for (size_t i = 0; i < prevprevlen; i++)
        if (prevprev[i] < 'A' || prevprev[i] > 'Z')
          return false;
    }
    prevprev = prev;
    prevprevlen = prevlen;
    prev = str;
    prevlen = tokenlen;
    str = GetNextToken(line, linelen, tokenlen, pos, 0);
  }
  if (!prev || !prevprev)
    return false;

  if (!IsNumeric(prev, prevlen) && (prevlen != 3 || memcmp(prev, "ANY", 3)))
    return false;

  if (!IsNumeric(prevprev, prevprevlen) && (prevprevlen != 3 || memcmp(prevprev, "ANY", 3)))
    return false;

  return true;
}

bool FtpListParser::parseTime(const char * str, size_t len, FtpListDate & date) const
{
  size_t i = 0;
  //Extract the hour
  date.HasTime = true;
  date.Hour = 0;
  while (str[i] != ':')
  {
    if (str[i] < '0' || str[i] > '9')
      return false;
    date.Hour *= 10;
    date.Hour += str[i] - '0';
    if (date.Hour > 24)
      return false;

    i++;
    if (i == len)
      return false;
  }
  // Make sure we did read at least one digit
  if (!i)
    return false;

  i++;

  if (i == len)
    return false;

  //Extract the minute
  date.Minute = 0;
  while (str[i] >= '0' && str[i] <= '9')
  {
    date.Minute *= 10;
    date.Minute += str[i] - '0';
    if (date.Minute > 59)
      return false;

    i++;
    if (i == len)
      break;
  }

  //Convert to 24h format
  if (i != len)
  {
    if (str[i] == 'P')
    {
      if (date.Hour < 12)
        date.Hour += 12;
    }
    else
      if (date.Hour == 12)
        date.Hour = 0;
  }

  date.HasTime = true;

  return true;
}

bool FtpListParser::parseAsWfFtp(const char * line, const size_t linelen, FtpListEntry & direntry)
{
  size_t pos = 0;
  size_t tokenlen = 0;

  const char * str = GetNextToken(line, linelen, tokenlen, pos, 0);
  if (!str)
    return false;

  direntry.Name = getStr(str, tokenlen);

  str = GetNextToken(line, linelen, tokenlen, pos, 0);
  if (!str)
    return false;

  if (!IsNumeric(str, tokenlen))
    return false;

  direntry.Size = strntoi64(str, tokenlen);

  str = GetNextToken(line, linelen, tokenlen, pos, 0);
  if (!str)
    return false;

  if (!ParseShortDate(str, tokenlen, direntry.Date))
    return false;

  str = GetNextToken(line, linelen, tokenlen, pos, 0);
  if (!str)
    return false;

  if (str[tokenlen - 1] != '.')
    return false;

  str = GetNextToken(line, linelen, tokenlen, pos, 0);
  if (!str)
    return false;

  if (!parseTime(str, tokenlen, direntry.Date))
    return false;

  direntry.Unsure = false;
  direntry.Dir = false;
  direntry.Link = false;
  direntry.Permissions.clear();
  direntry.OwnerGroup.clear();

  return true;
}

bool FtpListParser::ParseSize(const char * str, size_t len, qint64 & size) const
{
  if (len < 1)
    return false;

  if (IsNumeric(str, len))
  {
    size = strntoi64(str, len);
    return true;
  }

  size = 0;
  char last = str[--len];

  qint64 delimiter = -1;
  for (size_t i = 0; i < len; i++)
  {
    char c = str[i];
    if (c >= '0' && c <= '9')
    {
      size *= 10;
      size += c - '0';
    }
    else if (c == '.')
      delimiter = static_cast<qint64>(len - i);
    else
      return false;
  }

  // Check for digit before or after delimiterr
  if (!delimiter || delimiter == static_cast<qint64>(len))
    return false;

  switch (last)
  {
  case 'k':
  case 'K':
    size *= 1 << 10;
    break;
  case 'm':
  case 'M':
    size *= 1 << 20;
    break;
  case 'g':
  case 'G':
    size *= 1 << 30;
    break;
  case 't':
  case 'T':
    size *= static_cast<qint64>(1) << 40;
    break;
  default:
    return false;
  }

  if (delimiter == -1)
    return true;

  // (the original divided once more than there are decimal digits)
  while (--delimiter > 0)
    size /= 10;

  return true;
}

//---------------------------------------------------------------------------

bool FtpParsePermissions(const QString & APermissions, unsigned int & Mode)
{
  // TRights::Text / TRights::Octal as used by TFTPFileSystem::HandleListData
  QString Permissions = APermissions;
  if (Permissions.startsWith(QStringLiteral("0o")))
  {
    // Python style octal (pyftpdlib's unix.mode fact)
    Permissions = Permissions.mid(2);
  }
  qsizetype Len = Permissions.size();
  if (Len >= 10)
  {
    QString Text = Permissions.mid(1, 9);
    unsigned int Result = 0;
    static const unsigned int Bits[9] = { 0400, 0200, 0100, 040, 020, 010, 04, 02, 01 };
    for (int I = 0; I < 9; I++)
    {
      QChar C = Text[I];
      int Pos = I % 3;
      if (C == QLatin1Char('-'))
      {
        continue;
      }
      if ((Pos == 0) && (C == QLatin1Char('r')))
      {
        Result |= Bits[I];
      }
      else if ((Pos == 1) && (C == QLatin1Char('w')))
      {
        Result |= Bits[I];
      }
      else if (Pos == 2)
      {
        unsigned int Special = (I == 2) ? 04000u : ((I == 5) ? 02000u : 01000u);
        QChar Lower = C.toLower();
        bool SpecialChar = (I == 8) ? (Lower == QLatin1Char('t')) : (Lower == QLatin1Char('s'));
        if (C == QLatin1Char('x'))
        {
          Result |= Bits[I];
        }
        else if (SpecialChar)
        {
          Result |= Special;
          if (C.isLower())
          {
            Result |= Bits[I];
          }
        }
        else
        {
          return false;
        }
      }
      else
      {
        return false;
      }
    }
    Mode = Result;
    return true;
  }
  else if ((Len == 3) || (Len == 4))
  {
    unsigned int Result = 0;
    for (QChar C : Permissions)
    {
      if ((C < QLatin1Char('0')) || (C > QLatin1Char('7')))
      {
        return false;
      }
      Result = (Result << 3) | static_cast<unsigned int>(C.unicode() - '0');
    }
    Mode = Result & 07777;
    return true;
  }
  return false;
}

QDateTime FtpListDateToDateTime(const FtpListDate & Date)
{
  if (!Date.HasDate)
  {
    return QDateTime();
  }
  QDate D(Date.Year, Date.Month, Date.Day);
  if (!D.isValid())
  {
    return QDateTime();
  }
  QTime T(0, 0, 0);
  if (Date.HasTime)
  {
    T = QTime(Date.Hour % 24, Date.Minute, Date.HasSeconds ? Date.Second : 0);
    if (!T.isValid())
    {
      T = QTime(0, 0, 0);
    }
  }
  if (Date.Utc)
  {
    return QDateTime(D, T, QTimeZone::UTC);
  }
  return QDateTime(D, T);
}

FtpTimePrecision FtpListDatePrecision(const FtpListDate & Date)
{
  if (!Date.HasDate)
  {
    return ftpTimeNone;
  }
  if (!Date.HasTime)
  {
    return ftpTimeDay;
  }
  return Date.HasSeconds ? ftpTimeSecond : ftpTimeMinute;
}

RemoteFile FtpListEntryToRemoteFile(const FtpListEntry & Entry)
{
  RemoteFile File;
  File.FileName = Entry.Name;
  unsigned int Mode = 0;
  if (FtpParsePermissions(Entry.Permissions, Mode))
  {
    File.Permissions = Mode;
  }
  // deprecated, to be replaced with Owner/Group
  if (!Entry.OwnerGroup.isEmpty())
  {
    qsizetype Space = Entry.OwnerGroup.indexOf(QLatin1Char(' '));
    if (Space >= 0)
    {
      File.Owner = Entry.OwnerGroup.left(Space);
      File.Group = Entry.OwnerGroup.mid(Space + 1);
    }
    else
    {
      File.Owner = Entry.OwnerGroup;
    }
  }
  else
  {
    File.Owner = Entry.Owner;
    File.Group = Entry.Group;
  }
  File.Size = std::max<qint64>(Entry.Size, 0);
  if (Entry.Link)
  {
    File.FileType = RemoteFile::Symlink;
  }
  else if (Entry.Dir)
  {
    File.FileType = RemoteFile::Directory;
  }
  else
  {
    File.FileType = RemoteFile::File;
  }
  File.IsSymLink = Entry.Link;
  // FZAPI treats all symlink targets as directories (TFTPFileSystem::ReadSymlink)
  File.IsDirectory = Entry.Dir;
  if (File.IsDirectory)
  {
    File.Size = 0;
  }
  File.LinkTo = Entry.LinkTarget;
  File.Modification = FtpListDateToDateTime(Entry.Date);
  File.IsHidden = File.FileName.startsWith(QLatin1Char('.'));
  return File;
}

//---------------------------------------------------------------------------
namespace
{

// Windows-1252 characters of the bytes 0x80-0x9F (the 5 undefined ones map
// to the C1 control characters, so that every byte round-trips)
const char16_t Cp1252High[32] = {
  0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
  0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

}

QString FtpDecodeLegacy(const QByteArray & Bytes)
{
  QString Result;
  Result.reserve(Bytes.size());
  for (char C : Bytes)
  {
    unsigned char B = static_cast<unsigned char>(C);
    Result.append(((B >= 0x80) && (B < 0xA0)) ? QChar(Cp1252High[B - 0x80]) : QChar(B));
  }
  return Result;
}

QByteArray FtpEncodeLegacy(const QString & Str)
{
  QByteArray Result;
  Result.reserve(Str.size());
  for (QChar C : Str)
  {
    char16_t U = C.unicode();
    char B = '?';
    if ((U < 0x80) || ((U >= 0xA0) && (U <= 0xFF)))
    {
      B = static_cast<char>(U);
    }
    else
    {
      for (int Index = 0; Index < 32; Index++)
      {
        if (Cp1252High[Index] == U)
        {
          B = static_cast<char>(0x80 + Index);
          break;
        }
      }
    }
    Result.append(B);
  }
  return Result;
}

QString FtpTrimVmsVersion(const QString & FileName)
{
  // TTerminal::GetBaseFileName
  QString Result = FileName;
  qsizetype P = Result.lastIndexOf(QLatin1Char(';'));
  if (P >= 0)
  {
    Result.truncate(P);
  }
  return Result;
}
