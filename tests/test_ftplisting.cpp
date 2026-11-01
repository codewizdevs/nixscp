// Unit tests of core/FtpListing (port of WinSCP filezilla/FtpListResult.cpp).
// Listing samples are taken from the format list in FtpListResult.h and from
// the formats handled by the individual parser functions.
#include "core/FtpListing.h"

#include <QTimeZone>

#include <cstdio>

static int Failures = 0;
static int Checks = 0;

#define CHECK(Cond) \
  do { Checks++; if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); Failures++; } } while (0)
#define CHECK_EQ(A, B) \
  do { Checks++; auto VA = (A); auto VB = (B); if (!(VA == VB)) { std::printf("FAIL %s:%d: %s == %s (%s vs %s)\n", __FILE__, __LINE__, #A, #B, \
    qPrintable(QVariant::fromValue(VA).toString()), qPrintable(QVariant::fromValue(VB).toString())); Failures++; } } while (0)

#include <QVariant>

static const QDate Today(2024, 6, 15);

static QList<FtpListEntry> Parse(const QByteArray & Data, int ServerType = 0, bool Mlst = false, FtpListParser * Out = nullptr)
{
  FtpListParser Parser(Mlst, true, ServerType);
  Parser.SetCurrentDate(Today);
  Parser.AddData(Data);
  QList<FtpListEntry> Result = Parser.GetList();
  for (const QString & Line : Parser.UnparsedLines())
  {
    std::printf("  unparsed: %s\n", qPrintable(Line));
  }
  if (Out != nullptr)
  {
    *Out = Parser;
  }
  return Result;
}

static FtpListEntry One(const QByteArray & Line, int ServerType = 0, bool Mlst = false)
{
  QList<FtpListEntry> List = Parse(Line + "\r\n", ServerType, Mlst);
  if (List.size() != 1)
  {
    std::printf("FAIL: expected one entry for \"%s\", got %d\n", Line.constData(), static_cast<int>(List.size()));
    Failures++;
    return FtpListEntry();
  }
  return List[0];
}

static void CheckDate(int Line, const FtpListEntry & E, int Year, int Month, int Day, int Hour = -1, int Minute = -1)
{
  Checks++;
  bool Ok =
    E.Date.HasDate && (E.Date.Year == Year) && (E.Date.Month == Month) && (E.Date.Day == Day) &&
    ((Hour < 0) ? !E.Date.HasTime : (E.Date.HasTime && (E.Date.Hour == Hour) && (E.Date.Minute == Minute)));
  if (!Ok)
  {
    std::printf("FAIL line %d: date of '%s' is %d-%d-%d %d:%d (hasdate %d hastime %d), expected %d-%d-%d %d:%d\n",
      Line, qPrintable(E.Name), E.Date.Year, E.Date.Month, E.Date.Day, E.Date.Hour, E.Date.Minute,
      E.Date.HasDate, E.Date.HasTime, Year, Month, Day, Hour, Minute);
    Failures++;
  }
}
#define CHECK_DATE(E, ...) CheckDate(__LINE__, E, __VA_ARGS__)

static void TestUnix()
{
  FtpListEntry E = One("-rw-r--r--   1 root     other        531 Jan 29 03:26 README");
  CHECK_EQ(E.Name, QStringLiteral("README"));
  CHECK_EQ(E.Size, qint64(531));
  CHECK(!E.Dir && !E.Link);
  CHECK_EQ(E.OwnerGroup, QStringLiteral("root other"));
  CHECK_DATE(E, 2024, 1, 29, 3, 26);
  CHECK(!E.Date.Utc && !E.Date.HasYear);
  RemoteFile F = FtpListEntryToRemoteFile(E);
  CHECK_EQ(F.Permissions, 0644u);
  CHECK_EQ(F.Owner, QStringLiteral("root"));
  CHECK_EQ(F.Group, QStringLiteral("other"));
  CHECK(F.FileType == RemoteFile::File);
  CHECK_EQ(F.Modification, QDateTime(QDate(2024, 1, 29), QTime(3, 26)));

  E = One("dr-xr-xr-x   2 root     other        512 Apr  8  1994 etc");
  CHECK_EQ(E.Name, QStringLiteral("etc"));
  CHECK(E.Dir);
  CHECK_DATE(E, 1994, 4, 8);
  CHECK_EQ(FtpListEntryToRemoteFile(E).Permissions, 0555u);
  CHECK(FtpListEntryToRemoteFile(E).IsDirectory);

  // no group column
  E = One("dr-xr-xr-x   2 root     512 Apr  8  1994 etc");
  CHECK_EQ(E.Name, QStringLiteral("etc"));
  CHECK(E.Dir);
  CHECK_EQ(E.Size, qint64(512));
  CHECK_DATE(E, 1994, 4, 8);

  E = One("lrwxrwxrwx   1 root     other          7 Jan 25 00:17 bin -> usr/bin");
  CHECK_EQ(E.Name, QStringLiteral("bin"));
  CHECK_EQ(E.LinkTarget, QStringLiteral("usr/bin"));
  CHECK(E.Link && E.Dir);
  F = FtpListEntryToRemoteFile(E);
  CHECK(F.IsSymLink && F.IsDirectory && (F.FileType == RemoteFile::Symlink));
  CHECK_EQ(F.LinkTo, QStringLiteral("usr/bin"));

  // file in future of the "current" date -> previous year
  E = One("----------   1 owner    group         1803128 Jul 10 10:18 ls-lR.Z");
  CHECK_EQ(E.Name, QStringLiteral("ls-lR.Z"));
  CHECK_EQ(E.Size, qint64(1803128));
  CHECK_DATE(E, 2023, 7, 10, 10, 18);
  CHECK_EQ(FtpListEntryToRemoteFile(E).Permissions, 0u);

  E = One("d---------   1 owner    group               0 May  9 19:45 Softlib");
  CHECK_EQ(E.Name, QStringLiteral("Softlib"));
  CHECK(E.Dir);
  CHECK_DATE(E, 2024, 5, 9, 19, 45);

  E = One("-rwxrwxrwx   1 noone    nogroup      322 Aug 19  1996 message.ftp");
  CHECK_EQ(E.Name, QStringLiteral("message.ftp"));
  CHECK_EQ(E.Size, qint64(322));
  CHECK_DATE(E, 1996, 8, 19);
  CHECK_EQ(FtpListEntryToRemoteFile(E).Permissions, 0777u);

  // Numerical owner/group, size in group column position
  E = One("-------r--         326  1391972  1392298 Nov 22  1995 MegaPhone.sit");
  CHECK_EQ(E.Name, QStringLiteral("MegaPhone.sit"));
  CHECK_EQ(E.Size, qint64(1392298));
  CHECK_DATE(E, 1995, 11, 22);

  // NetPresenz
  E = One("drwxrwxr-x               folder        2 May 10  1996 network");
  CHECK_EQ(E.Name, QStringLiteral("network"));
  CHECK(E.Dir);
  CHECK_DATE(E, 1996, 5, 10);

  // Names with spaces and trailing indicator
  E = One("-rw-r--r--   1 user     group        100 Jan  1  2020 file with  spaces.txt");
  CHECK_EQ(E.Name, QStringLiteral("file with  spaces.txt"));
  E = One("-rwxr-xr-x   1 user     group        100 Jan  1  2020 script.sh*");
  CHECK_EQ(E.Name, QStringLiteral("script.sh"));
  CHECK_EQ(FtpListEntryToRemoteFile(E).Permissions, 0755u);

  // Special bits
  CHECK_EQ(FtpListEntryToRemoteFile(One("drwxrwxrwt   9 root     root      4096 Jun  1 10:00 tmp")).Permissions, 01777u);
  CHECK_EQ(FtpListEntryToRemoteFile(One("-rwsr-xr-x   1 root     root     54096 Jun  1 10:00 passwd")).Permissions, 04755u);
  CHECK_EQ(FtpListEntryToRemoteFile(One("-rwSr-Sr-T   1 root     root     54096 Jun  1 10:00 odd")).Permissions, 07644u);
  // ACL marker
  CHECK_EQ(FtpListEntryToRemoteFile(One("-rw-r--r--+  1 root     root        10 Jun  1 10:00 acl")).Permissions, 0644u);

  // ISO dates
  E = One("-rw-r--r--   1 user     group       1234 2002-10-14 12:34 isodate");
  CHECK_EQ(E.Name, QStringLiteral("isodate"));
  CHECK_EQ(E.Size, qint64(1234));
  CHECK_DATE(E, 2002, 10, 14, 12, 34);
  E = One("-rw-r--r--   1 user     group       1234 14-10-2002 12:34 eurodate");
  CHECK_DATE(E, 2002, 10, 14, 12, 34);
  E = One("-rw-r--r--   1 user     group       1234 01-jun-99 12:34 shortyear");
  CHECK_EQ(E.Name, QStringLiteral("shortyear"));
  CHECK_EQ(E.Date.Month, 6);
  CHECK_EQ(E.Date.Day, 1);
  E = One("-rw-r--r--   1 user     group       1234 2002/10/14 12:34 slashdate");
  CHECK_DATE(E, 2002, 10, 14, 12, 34);
  E = One("-rw-r--r--   1 user     group       1234 14/10/2002 12:34 slashdate2");
  CHECK_DATE(E, 2002, 10, 14, 12, 34);

  // Day before month, localized month names
  E = One("-rw-r--r--   1 user     group       1234 10 Mar  2003 daymonth");
  CHECK_EQ(E.Name, QStringLiteral("daymonth"));
  CHECK_DATE(E, 2003, 3, 10);
  E = One("-rw-r--r--   1 user     group       1234 Mrz 10  2003 german");
  CHECK_DATE(E, 2003, 3, 10);
  E = One("-rw-r--r--   1 user     group       1234 okt 10  2003 german2");
  CHECK_DATE(E, 2003, 10, 10);
  // UTF-8 "fév" is not in the table (only Latin-1), as in WinSCP
  CHECK(Parse("-rw-r--r--   1 user     group       1234 f\xC3\xA9v 10  2003 french\r\n").isEmpty());
  E = One("-rw-r--r--   1 user     group       1234 f\xE9v 10  2003 french");
  CHECK_DATE(E, 2003, 2, 10);
  // UTF-8 Japanese/Chinese "3月": digits followed by non-ASCII
  E = One("-rw-r--r--   1 user     group       1234 3\xE6\x9C\x88 10 12:00 cjk");
  CHECK_EQ(E.Name, QStringLiteral("cjk"));
  CHECK_DATE(E, 2024, 3, 10, 12, 0);
  E = One("-rw-r--r--   1 user     group       1234 10\xE6\x9C\x88 1\xE6\x97\xA5 12:00 cjk2");
  CHECK_EQ(E.Name, QStringLiteral("cjk2"));
  CHECK_DATE(E, 2023, 10, 1, 12, 0);

  // Size with unit
  E = One("-rw-r--r--   1 user     group       1.5k Jan  1  2020 withunit");
  CHECK_EQ(E.Name, QStringLiteral("withunit"));
  CHECK_EQ(E.Size, qint64(1536));
  E = One("-rw-r--r--   1 user     group         2M Jan  1  2020 withunit2");
  CHECK_EQ(E.Size, qint64(2 * 1024 * 1024));

  // Seconds in time
  E = One("-rw-r--r--   1 user     group       1234 Jan  2 12:34:56 secs");
  CHECK(E.Date.HasSeconds);
  CHECK_EQ(E.Date.Second, 56);

  // Three tags (domain, group, user)
  E = One("-rw-r--r--   1 domain   group      user        1234 Jan  1  2020 threetags");
  CHECK_EQ(E.Name, QStringLiteral("threetags"));
  CHECK_EQ(E.Size, qint64(1234));

  // VShell: year and time
  E = One("-rwxr-xr-x    1 user     group           1234 Oct 21, 2003 15:03 vshell");
  CHECK_EQ(E.Name, QStringLiteral("vshell"));
  CHECK_DATE(E, 2003, 10, 21, 15, 3);

  // UTF-8 name, non-UTF-8 name
  E = One("-rw-r--r--   1 user     group       1234 Jan  1  2020 \xC5\xBElu\xC5\xA5ou\xC4\x8Dk\xC3\xBD");
  CHECK_EQ(E.Name, QString::fromUtf8("\xC5\xBElu\xC5\xA5ou\xC4\x8Dk\xC3\xBD"));
  FtpListParser Latin(false, true);
  Latin.AddData(QByteArray("-rw-r--r--   1 user     group       1234 Jan  1  2020 caf\xE9\n"));
  QList<FtpListEntry> L = Latin.GetList();
  CHECK((L.size() == 1) && (L[0].Name == QString::fromUtf8("caf\xC3\xA9")));
  CHECK(!Latin.Utf8());
}

static void TestNetWare()
{
  FtpListEntry E = One("d [R----F--] supervisor            512       Jan 16 18:53    login");
  CHECK_EQ(E.Name, QStringLiteral("login"));
  CHECK(E.Dir);
  CHECK_EQ(E.Size, qint64(512));
  CHECK_EQ(E.OwnerGroup, QStringLiteral("supervisor"));
  CHECK_DATE(E, 2024, 1, 16, 18, 53);

  E = One("- [R----F--] rhesus             214059       Oct 20 15:27    cx.exe");
  CHECK_EQ(E.Name, QStringLiteral("cx.exe"));
  CHECK(!E.Dir);
  CHECK_EQ(E.Size, qint64(214059));
  CHECK_DATE(E, 2023, 10, 20, 15, 27);
}

static void TestVms()
{
  FtpListParser Parser;
  Parser.SetCurrentDate(Today);
  Parser.AddData(QByteArray(
    "00README.TXT;1      2 30-DEC-1996 17:44 [SYSTEM] (RWED,RWED,RE,RE)\r\n"
    "00README.TXT;3      4 31-DEC-1996 17:45 [SYSTEM] (RWED,RWED,RE,RE)\r\n"
    "CORE.DIR;1          1  8-SEP-1996 16:09 [SYSTEM] (RWE,RWE,RE,RE)\r\n"
    "CII-MANUAL.TEX;1  213/216  29-JAN-1996 03:33:12  [ANONYMOU,ANONYMOUS]   (RWED,RWED,,)\r\n"
    // multi-line entry
    "LONGFILENAME-WITH-LONG-NAME.TXT;2\r\n"
    "                    5 02-FEB-1999 11:11 [USER] (RWED,RWED,,)\r\n"));
  QList<FtpListEntry> L = Parser.GetList();
  CHECK(Parser.ServerType() & FtpListParser::stVMS);
  CHECK_EQ(L.size(), qsizetype(4));
  if (L.size() == 4)
  {
    CHECK_EQ(L[0].Name, QStringLiteral("00README.TXT"));
    CHECK_EQ(L[0].Size, qint64(4 * 512));   // latest version kept
    CHECK_DATE(L[0], 1996, 12, 31, 17, 45);
    CHECK_EQ(L[0].OwnerGroup, QStringLiteral("SYSTEM"));
    CHECK_EQ(L[0].Permissions, QStringLiteral("RWED,RWED,RE,RE"));
    CHECK_EQ(L[1].Name, QStringLiteral("CORE"));
    CHECK(L[1].Dir);
    CHECK_DATE(L[1], 1996, 9, 8, 16, 9);
    CHECK_EQ(L[2].Name, QStringLiteral("CII-MANUAL.TEX"));
    CHECK_EQ(L[2].Size, qint64(213 * 512));
    CHECK_DATE(L[2], 1996, 1, 29, 3, 33);
    CHECK_EQ(L[3].Name, QStringLiteral("LONGFILENAME-WITH-LONG-NAME.TXT"));
    CHECK_EQ(L[3].Size, qint64(5 * 512));
  }
}

static void TestDos()
{
  QList<FtpListEntry> L = Parse(
    "04-27-00  09:09PM       <DIR>          licensed\r\n"
    "07-18-00  10:16AM       <DIR>          pub\r\n"
    "04-14-00  03:47PM                  589 readme.htm\r\n"
    "12-05-2019  12:30AM          1,234,567 big file.zip\r\n");
  CHECK_EQ(L.size(), qsizetype(4));
  if (L.size() == 4)
  {
    CHECK_EQ(L[0].Name, QStringLiteral("licensed"));
    CHECK(L[0].Dir);
    CHECK_DATE(L[0], 2000, 4, 27, 21, 9);
    CHECK_EQ(L[1].Name, QStringLiteral("pub"));
    CHECK_DATE(L[1], 2000, 7, 18, 10, 16);
    CHECK_EQ(L[2].Name, QStringLiteral("readme.htm"));
    CHECK(!L[2].Dir);
    CHECK_EQ(L[2].Size, qint64(589));
    CHECK_DATE(L[2], 2000, 4, 14, 15, 47);
    CHECK_EQ(L[3].Name, QStringLiteral("big file.zip"));
    CHECK_EQ(L[3].Size, qint64(1234567));
    CHECK_DATE(L[3], 2019, 12, 5, 0, 30);
  }
  // 24h format, dd.mm.yyyy
  FtpListEntry E = One("31.12.2010  23:59                 100 euro.txt");
  CHECK_EQ(E.Name, QStringLiteral("euro.txt"));
  CHECK_DATE(E, 2010, 12, 31, 23, 59);
}

static void TestMlsd()
{
  QList<FtpListEntry> L = Parse(
    "type=cdir;modify=20200101000000;perm=flcdmpe; .\r\n"
    "type=pdir;modify=20200101000000;perm=flcdmpe; ..\r\n"
    "type=file;size=531;modify=20010129032600;UNIX.mode=0644;UNIX.owner=root;UNIX.group=other;perm=adfrw; README\r\n"
    "type=dir;modify=20200229235959.123;unix.mode=755;unix.uid=1000;unix.gid=100; pub dir\r\n"
    "Type=OS.unix=slink:/foobar;Perm=;Unique=keVO1+4G4; foobar\r\n"
    "type=OS.unix=symlink;size=4;modify=20200101000000; link\r\n"
    "modify=20200101000000;type=file;size=0;unix.ownername=alice;unix.owner=1000; empty\r\n"
    "type=file;size=10;create=19991231120000; created only\r\n");
  CHECK_EQ(L.size(), qsizetype(6));
  if (L.size() == 6)
  {
    CHECK_EQ(L[0].Name, QStringLiteral("README"));
    CHECK_EQ(L[0].Size, qint64(531));
    CHECK(!L[0].Dir);
    CHECK(L[0].Date.Utc && L[0].Date.HasSeconds);
    CHECK_DATE(L[0], 2001, 1, 29, 3, 26);
    CHECK_EQ(L[0].Owner, QStringLiteral("root"));
    CHECK_EQ(L[0].Group, QStringLiteral("other"));
    CHECK_EQ(L[0].HumanPermissions, QStringLiteral("adfrw"));
    RemoteFile F = FtpListEntryToRemoteFile(L[0]);
    CHECK_EQ(F.Permissions, 0644u);
    CHECK_EQ(F.Modification, QDateTime(QDate(2001, 1, 29), QTime(3, 26, 0), QTimeZone::UTC));
    CHECK_EQ(L[1].Name, QStringLiteral("pub dir"));
    CHECK(L[1].Dir);
    CHECK_EQ(L[1].Date.Second, 59);
    CHECK_EQ(L[1].Owner, QStringLiteral("1000"));
    CHECK_EQ(FtpListEntryToRemoteFile(L[1]).Permissions, 0755u);
    CHECK_EQ(L[2].Name, QStringLiteral("foobar"));
    CHECK(L[2].Link);
    CHECK_EQ(L[2].LinkTarget, QStringLiteral("/foobar"));
    CHECK_EQ(L[3].Name, QStringLiteral("link"));
    CHECK(L[3].Link);
    CHECK_EQ(L[4].Name, QStringLiteral("empty"));
    CHECK_EQ(L[4].Owner, QStringLiteral("alice"));
    CHECK_EQ(L[5].Name, QStringLiteral("created only"));
    CHECK_DATE(L[5], 1999, 12, 31, 12, 0);
  }
  // Strict: a malformed fact list is not MLSD (and falls through to other parsers)
  FtpListEntry E;
  FtpListParser Parser;
  CHECK(!Parser.ParseLine("type=file;size=abc; name", E) || (E.Name != QStringLiteral("name")));

  // MLST: the name is a path
  E = One("type=file;size=10;modify=20200101000000; /home/user/file.txt", 0, true);
  CHECK_EQ(E.Name, QStringLiteral("file.txt"));
  CHECK_EQ(E.Size, qint64(10));
  E = One("type=cdir;modify=20200101000000; /home/user/", 0, true);
  CHECK_EQ(E.Name, QStringLiteral("user"));
  CHECK(E.Dir);
}

static void TestEplf()
{
  QList<FtpListEntry> L = Parse(
    "+i8388621.29609,m824255902,/,\tdev\r\n"
    "+i8388621.44468,m839956783,r,s10376,up644,\tRFCEPLF\r\n");
  CHECK_EQ(L.size(), qsizetype(2));
  if (L.size() == 2)
  {
    CHECK_EQ(L[0].Name, QStringLiteral("dev"));
    CHECK(L[0].Dir);
    QDateTime T = QDateTime::fromSecsSinceEpoch(824255902, QTimeZone::UTC);
    CHECK_DATE(L[0], T.date().year(), T.date().month(), T.date().day(), T.time().hour(), T.time().minute());
    CHECK_EQ(L[1].Name, QStringLiteral("RFCEPLF"));
    CHECK(!L[1].Dir);
    CHECK_EQ(L[1].Size, qint64(10376));
    CHECK_EQ(FtpListEntryToRemoteFile(L[1]).Permissions, 0644u);
  }
}

static void TestOther()
{
  // Numerical Unix format
  FtpListEntry E = One("100644   1  1000  12345 1020304050 numeric.txt");
  CHECK_EQ(E.Name, QStringLiteral("numeric.txt"));
  CHECK_EQ(E.Size, qint64(12345));
  CHECK(!E.Dir);
  QDateTime T = QDateTime::fromSecsSinceEpoch(1020304050, QTimeZone::UTC);
  CHECK_DATE(E, T.date().year(), T.date().month(), T.date().day(), T.time().hour(), T.time().minute());
  E = One("040755   2  1000  4096 1020304050 numdir");
  CHECK(E.Dir);

  // OS/2
  QList<FtpListEntry> L = Parse(
    "     0           DIR   04-11-95   16:26  ADDRESS\r\n"
    "   612      A          07-28-95   16:45  air_tra1.bag\r\n"
    "   195      A          08-09-95   10:23  Alfa1.bag\r\n");
  CHECK_EQ(L.size(), qsizetype(3));
  if (L.size() == 3)
  {
    CHECK_EQ(L[0].Name, QStringLiteral("ADDRESS"));
    CHECK(L[0].Dir);
    CHECK_DATE(L[0], 1995, 4, 11, 16, 26);
    CHECK_EQ(L[1].Name, QStringLiteral("air_tra1.bag"));
    CHECK_EQ(L[1].Size, qint64(612));
    CHECK(!L[1].Dir);
    CHECK_EQ(L[2].Name, QStringLiteral("Alfa1.bag"));
  }

  // Month first, size first (VShell/Windows variant)
  E = One("    1234 Oct 21, 2003 15:03 other.txt");
  CHECK_EQ(E.Name, QStringLiteral("other.txt"));
  CHECK_EQ(E.Size, qint64(1234));
  CHECK_DATE(E, 2003, 10, 21, 15, 3);
  E = One("       0 Oct 21 03 15:03 subdir/");
  CHECK_EQ(E.Name, QStringLiteral("subdir"));
  CHECK(E.Dir);
  CHECK_DATE(E, 2003, 10, 21, 15, 3);
}

static void TestIbm()
{
  // AS/400
  FtpListEntry E = One("QSYS            77824 02/23/00 15:09:55 *DIR       QSYS.LIB/");
  CHECK_EQ(E.Name, QStringLiteral("QSYS.LIB"));
  CHECK(E.Dir);
  CHECK_EQ(E.Size, qint64(77824));
  CHECK_DATE(E, 2000, 2, 23, 15, 9);
  E = One("QDOC            24576 09/24/99 23:12:12 *FLR       /QDLS/QDOC/");
  CHECK(E.Dir);

  // MVS datasets
  E = One("SAVE00 3390   2004/06/23  1    1  FB     128  6144  PS    INCOMING.RPTBM023.D061704");
  CHECK_EQ(E.Name, QStringLiteral("INCOMING.RPTBM023.D061704"));
  CHECK(!E.Dir);
  E = One("SAVE01 3390   2004/06/23  1    1  FB     128 27648  PO    PDS.DATASET");
  CHECK_EQ(E.Name, QStringLiteral("PDS.DATASET"));
  CHECK(E.Dir);
  E = One("Migrated                                                         SOME.MIGRATED.NAME");
  CHECK_EQ(E.Name, QStringLiteral("SOME.MIGRATED.NAME"));
  E = One("TSO004 3390 VSAM FOO.BAR");
  CHECK_EQ(E.Name, QStringLiteral("FOO.BAR"));

  // MVS PDS members
  E = One("TBSHELF   01.03 2002/09/12 2002/10/11 09:37    11    11     0 KK");
  CHECK_EQ(E.Name, QStringLiteral("TBSHELF"));
  CHECK_EQ(E.Size, qint64(11));
  CHECK_DATE(E, 2002, 10, 11, 9, 37);

  // MVS PDS members without ISPF statistics (only with MVS server type)
  E = One("MEMBER1   00000C 0000A0 00 FO 31 ANY", FtpListParser::stMVS);
  CHECK_EQ(E.Name, QStringLiteral("MEMBER1"));
  CHECK_EQ(E.Size, qint64(12));
  CHECK(Parse("MEMBER1   00000C 0000A0 00 FO 31 ANY\r\n").isEmpty());
  E = One("NAMEONLY            ", FtpListParser::stMVS);
  CHECK_EQ(E.Name, QStringLiteral("NAMEONLY"));
}

static void TestWfFtp()
{
  FtpListEntry E = One("wfftp.txt 123 12.10.2004 mon. 09:30");
  CHECK_EQ(E.Name, QStringLiteral("wfftp.txt"));
  CHECK_EQ(E.Size, qint64(123));
  CHECK_DATE(E, 2004, 10, 12, 9, 30);
}

static void TestBuffering()
{
  // "total" line skipped, chunks split anywhere (including between CR and LF),
  // last line without terminator
  QByteArray Data =
    "total 12\r\n"
    "-rw-r--r--   1 user     group        100 Jan  1  2020 a.txt\r\n"
    "drwxr-xr-x   2 user     group       4096 Jan  1  2020 .\r\n"
    "drwxr-xr-x   2 user     group       4096 Jan  1  2020 ..\r\n"
    "drwxr-xr-x   2 user     group       4096 Jan  1  2020 .hidden\r\n"
    "-rw-r--r--   1 user     group        200 Jan  1  2020 b.txt";
  for (int ChunkSize = 1; ChunkSize <= 7; ChunkSize++)
  {
    FtpListParser Parser;
    for (qsizetype Pos = 0; Pos < Data.size(); Pos += ChunkSize)
    {
      Parser.AddData(Data.mid(Pos, ChunkSize));
    }
    QList<FtpListEntry> L = Parser.GetList();
    CHECK_EQ(L.size(), qsizetype(3));
    if (L.size() == 3)
    {
      CHECK_EQ(L[0].Name, QStringLiteral("a.txt"));
      CHECK_EQ(L[1].Name, QStringLiteral(".hidden"));
      CHECK(FtpListEntryToRemoteFile(L[1]).IsHidden);
      CHECK_EQ(L[2].Name, QStringLiteral("b.txt"));
      CHECK_EQ(L[2].Size, qint64(200));
    }
    CHECK_EQ(Parser.UnparsedLines().size(), qsizetype(1));
  }
  // LF only, empty lines
  QList<FtpListEntry> L = Parse("\n\n-rw-r--r-- 1 u g 1 Jan 1 2020 x\n\n-rw-r--r-- 1 u g 2 Jan 1 2020 y\n");
  CHECK_EQ(L.size(), qsizetype(2));
  // Empty listing
  CHECK(Parse("").isEmpty());
}

static void TestHelpers()
{
  unsigned int Mode = 0;
  CHECK(FtpParsePermissions(QStringLiteral("0755"), Mode) && (Mode == 0755));
  CHECK(FtpParsePermissions(QStringLiteral("644"), Mode) && (Mode == 0644));
  CHECK(FtpParsePermissions(QStringLiteral("0o775"), Mode) && (Mode == 0775));
  CHECK(!FtpParsePermissions(QStringLiteral("RWED"), Mode));
  CHECK(!FtpParsePermissions(QStringLiteral("drwxr-xr-q"), Mode));
  FtpListDate D;
  CHECK(!FtpListDateToDateTime(D).isValid());
  CHECK(FtpListDatePrecision(D) == ftpTimeNone);
  D.HasDate = true;
  D.Year = 2020;
  D.Month = 2;
  D.Day = 29;
  CHECK(FtpListDatePrecision(D) == ftpTimeDay);
  CHECK_EQ(FtpListDateToDateTime(D), QDateTime(QDate(2020, 2, 29), QTime(0, 0)));
}

int main()
{
  TestUnix();
  TestNetWare();
  TestVms();
  TestDos();
  TestMlsd();
  TestEplf();
  TestOther();
  TestIbm();
  TestWfFtp();
  TestBuffering();
  TestHelpers();
  std::printf("%d checks, %d failures\n", Checks, Failures);
  return (Failures == 0) ? 0 : 1;
}
