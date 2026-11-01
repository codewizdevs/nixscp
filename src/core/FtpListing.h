// Port of WinSCP source/filezilla/FtpListResult.{h,cpp} (CFtpListResult, the
// FileZilla 2 directory listing parser) and of the entry conversion in
// source/core/FtpFileSystem.cpp (TFTPFileSystem::HandleListData,
// TFTPFileSystem::RemoteFileTimeToDateTimeAndPrecision).
//
// Supported formats (as in the original):
//   -rw-r--r--   1 root     other        531 Jan 29 03:26 README
//   dr-xr-xr-x   2 root     other        512 Apr  8  1994 etc
//   dr-xr-xr-x   2 root     512 Apr  8  1994 etc
//   lrwxrwxrwx   1 root     other          7 Jan 25 00:17 bin -> usr/bin
//   d [R----F--] supervisor            512       Jan 16 18:53    login
//   -------r--         326  1391972  1392298 Nov 22  1995 MegaPhone.sit
//   drwxrwxr-x               folder        2 May 10  1996 network
//   00README.TXT;1      2 30-DEC-1996 17:44 [SYSTEM] (RWED,RWED,RE,RE)
//   CII-MANUAL.TEX;1  213/216  29-JAN-1996 03:33:12  [ANONYMOU,ANONYMOUS]   (RWED,RWED,,)
//   04-27-00  09:09PM       <DIR>          licensed
//   04-14-00  03:47PM                  589 readme.htm
//   type=file;size=531;modify=20010129032600; README         (MLSD/MLST, RFC 3659)
//   +i8388621.29609,m824255902,/,  dev                       (EPLF)
// plus OS/2, numerical Unix, VShell, IBM, MVS, BS2000 and WfFtp variants.
#pragma once

#include "core/RemoteFile.h"

#include <QByteArray>
#include <QDate>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

#include <map>
#include <string>

// t_directory::t_direntry::t_date
struct FtpListDate
{
  int Year = 0;
  int Month = 0;
  int Day = 0;
  int Hour = 0;
  int Minute = 0;
  int Second = 0;
  bool HasTime = false;
  bool HasYear = false;     // ignored and assumed true when HasSeconds
  bool HasSeconds = false;
  bool HasDate = false;
  bool Utc = false;
};

// t_directory::t_direntry
struct FtpListEntry
{
  bool Unsure = true;
  QString Name;
  QString Permissions;
  QString HumanPermissions;  // MLSD "perm" fact (RFC format)
  QString OwnerGroup;        // deprecated, to be replaced with Owner/Group
  QString Owner;
  QString Group;
  qint64 Size = 0;
  bool Dir = false;
  bool Link = false;
  FtpListDate Date;
  QString LinkTarget;
};

class FtpListParser
{
public:
  // FZ_SERVERTYPE_SUB_FTP_* flags
  enum ServerTypeFlag { stVMS = 0x01, stMVS = 0x02, stBS2000 = 0x04 };

  // Mlst: parsing a single MLST reply line (path in the name)
  // Utf8: names are expected to be UTF-8 (falls back to Latin-1 when invalid)
  explicit FtpListParser(bool Mlst = false, bool Utf8 = true, int ServerType = 0, bool VmsAllRevisions = false);

  // Feed raw listing data; may be called repeatedly with arbitrary chunks
  void AddData(const char * Data, qsizetype Size);
  void AddData(const QByteArray & Data) { AddData(Data.constData(), Data.size()); }
  // Returns the parsed entries ("." and ".." excluded). A last line without
  // line terminator is parsed too (the original dropped it).
  QList<FtpListEntry> GetList();

  // Lines that could not be parsed (for logging)
  const QStringList & UnparsedLines() const { return FUnparsed; }
  // Server type flags, stVMS is added when VMS listing is detected
  int ServerType() const { return FServerType; }
  // False after names in invalid UTF-8 were seen (CFtpControlSocket::DecodeString)
  bool Utf8() const { return FUtf8; }
  // Reference date used to guess the year of "Mon DD HH:MM" entries (tests)
  void SetCurrentDate(const QDate & Date) { FCurrentDate = Date; }

  // Parses a single line (no buffering); for tests and MLST
  bool ParseLine(const QByteArray & Line, FtpListEntry & Entry);

private:
  bool FMlst;
  bool FUtf8;
  int FServerType;
  bool FVmsAllRevisions;
  QDate FCurrentDate;
  std::string FBuffer;
  QList<FtpListEntry> FEntries;
  QList<int> FVersions;   // m_TempData
  QStringList FUnparsed;
  std::map<std::string, int> FMonthNames;

  bool parseLine(const char * lineToParse, const size_t linelen, FtpListEntry & direntry);
  bool parseAsVMS(const char * line, const size_t linelen, FtpListEntry & direntry);
  bool parseAsEPLF(const char * line, const size_t linelen, FtpListEntry & direntry);
  bool parseAsMlsd(const char * line, const size_t linelen, FtpListEntry & direntry);
  bool parseAsUnix(const char * line, const size_t linelen, FtpListEntry & direntry);
  bool parseAsDos(const char * line, const size_t linelen, FtpListEntry & direntry);
  bool parseAsOther(const char * line, const size_t linelen, FtpListEntry & direntry);
  bool parseAsIBM(const char * line, const size_t linelen, FtpListEntry & direntry);
  bool parseAsIBMMVS(const char * line, const size_t linelen, FtpListEntry & direntry);
  bool parseAsIBMMVSPDS(const char * line, const size_t linelen, FtpListEntry & direntry);
  bool parseAsIBMMVSPDS2(const char * line, const size_t linelen, FtpListEntry & direntry);
  bool parseAsWfFtp(const char * line, const size_t linelen, FtpListEntry & direntry);

  const char * GetNextToken(const char * line, const size_t linelen, size_t & len, size_t & pos, int type) const;
  bool ParseShortDate(const char * str, size_t len, FtpListDate & date) const;
  bool parseTime(const char * str, size_t len, FtpListDate & date) const;
  bool ParseSize(const char * str, size_t len, qint64 & size) const;
  void TimeTToDate(qint64 TimeT, FtpListDate & date) const;
  void GuessYearIfUnknown(FtpListDate & Date) const;
  bool parseMlsdDateTime(const QString & value, FtpListDate & date) const;
  int FindMonth(const char * str, size_t len) const;

  QString getStr(const char * source, size_t len, bool mayInvalidateUTF8 = false);
  const char * strnchr(const char * str, size_t len, char c) const;
  const char * strnstr(const char * str, size_t len, const char * c) const;
  qint64 strntoi64(const char * str, size_t len) const;
  void AddLine(FtpListEntry & direntry);
  bool IsNumeric(const char * str, size_t len) const;
  static bool IsNewLineChar(char C);
};

// Unix rights from listing permission text ("drwxr-xr-x", "-rw-r--r--+",
// octal "755"/"0644" as in MLSD unix.mode); returns false when not recognized.
bool FtpParsePermissions(const QString & Permissions, unsigned int & Mode);

// RemoteFileTimeToDateTimeAndPrecision: invalid QDateTime when there is no date.
// UTC timestamps (MLSD, MDTM) are returned as UTC, others as local time.
QDateTime FtpListDateToDateTime(const FtpListDate & Date);
// Precision of the listing timestamp (TModificationFmt)
enum FtpTimePrecision { ftpTimeNone, ftpTimeDay, ftpTimeMinute, ftpTimeSecond };
FtpTimePrecision FtpListDatePrecision(const FtpListDate & Date);

// TFTPFileSystem::HandleListData. As in WinSCP, symlinks are reported as
// directories unless the caller resolves them.
RemoteFile FtpListEntryToRemoteFile(const FtpListEntry & Entry);

// Legacy (non UTF-8) charset of FTP file names and commands. WinSCP uses
// the Windows ANSI code page; Linux locales are UTF-8 (Qt 6 assumes so),
// so the Western ANSI code page Windows-1252 (a superset of Latin-1) is
// used. Every byte value round-trips.
QString FtpDecodeLegacy(const QByteArray & Bytes);
QByteArray FtpEncodeLegacy(const QString & Str);

// TTerminal::GetBaseFileName: "FILE.TXT;3" -> "FILE.TXT" (the VMS version
// number is trimmed from local names of downloads when
// SessionData::TrimVMSVersions is on)
QString FtpTrimVmsVersion(const QString & FileName);
