// Unit tests for core/FileMasks (port of WinSCP TFileMasks) and core/CopyParam
// Examples follow https://winscp.net/eng/docs/file_mask
#include "core/FileMasks.h"
#include "core/CopyParam.h"
#include "core/IniStorage.h"

#include <QTemporaryDir>
#include <cstdio>

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); Failures++; } } while (0)

// Matches a full path (file)
static bool F(const QString & Masks, const QString & Path, qint64 Size = 0, const QDateTime & Time = QDateTime::currentDateTime())
{
  FileMasks M(Masks);
  FileMasks::Params P;
  P.Size = Size;
  P.Modification = Time;
  return M.Matches(Path, false, false, &P);
}

// Matches a full path (directory)
static bool D(const QString & Masks, const QString & Path)
{
  FileMasks M(Masks);
  FileMasks::Params P;
  return M.Matches(Path, false, true, &P);
}

static bool Valid(const QString & Masks, int * ErrorStart = nullptr)
{
  try
  {
    FileMasks M(Masks);
    return true;
  }
  catch (const FileMasksException & E)
  {
    if (ErrorStart != nullptr)
    {
      *ErrorStart = E.ErrorStart;
    }
    return false;
  }
}

static void TestWildcards()
{
  // Wildcards: * ? and sets
  CHECK(F(QStringLiteral("*.txt"), QStringLiteral("/home/a.txt")));
  CHECK(F(QStringLiteral("*.txt"), QStringLiteral("/home/A.TXT")));   // case insensitive
  CHECK(!F(QStringLiteral("*.txt"), QStringLiteral("/home/a.txt.bak")));
  CHECK(F(QStringLiteral("*.???"), QStringLiteral("/a.doc")));
  CHECK(!F(QStringLiteral("*.???"), QStringLiteral("/a.docx")));
  CHECK(F(QStringLiteral("*.?"), QStringLiteral("/a.c")));
  CHECK(!F(QStringLiteral("*.?"), QStringLiteral("/a.cc")));
  CHECK(F(QStringLiteral("*.[ch]"), QStringLiteral("/main.c")));
  CHECK(F(QStringLiteral("*.[ch]"), QStringLiteral("/main.H")));
  CHECK(!F(QStringLiteral("*.[ch]"), QStringLiteral("/main.o")));
  CHECK(F(QStringLiteral("[a-c]*"), QStringLiteral("/beta")));
  CHECK(!F(QStringLiteral("[a-c]*"), QStringLiteral("/delta")));
  CHECK(F(QStringLiteral("[!a-c]*"), QStringLiteral("/delta")));
  CHECK(!F(QStringLiteral("[!a-c]*"), QStringLiteral("/alpha")));
  CHECK(F(QStringLiteral("a*b*c"), QStringLiteral("/aXXbYYbZc")));
  CHECK(!F(QStringLiteral("a*b*c"), QStringLiteral("/aXXbYYbZ")));
  // "*", "*.*" and empty mask match anything, "*." files without extension
  CHECK(F(QStringLiteral("*"), QStringLiteral("/noext")));
  CHECK(F(QStringLiteral("*.*"), QStringLiteral("/noext")));
  CHECK(F(QString(), QStringLiteral("/noext")));
  CHECK(F(QStringLiteral("*."), QStringLiteral("/Makefile")));
  CHECK(!F(QStringLiteral("*."), QStringLiteral("/a.txt")));
  // exact names
  CHECK(F(QStringLiteral(".htaccess"), QStringLiteral("/www/.htaccess")));
  CHECK(!F(QStringLiteral(".htaccess"), QStringLiteral("/www/x.htaccess")));
}

static void TestLists()
{
  // Multiple masks separated by ';' or ','
  CHECK(F(QStringLiteral("*.txt; *.log"), QStringLiteral("/x.log")));
  CHECK(F(QStringLiteral("*.txt,*.log"), QStringLiteral("/x.txt")));
  CHECK(!F(QStringLiteral("*.txt; *.log"), QStringLiteral("/x.doc")));
  // Include | exclude
  CHECK(!F(QStringLiteral("| *.bak"), QStringLiteral("/x.bak")));
  CHECK(F(QStringLiteral("| *.bak"), QStringLiteral("/x.txt")));
  CHECK(F(QStringLiteral("*.txt | readme*"), QStringLiteral("/notes.txt")));
  CHECK(!F(QStringLiteral("*.txt | readme*"), QStringLiteral("/readme.txt")));
  CHECK(!F(QStringLiteral("*.txt | readme*"), QStringLiteral("/a.doc")));
  // Doubled delimiter is a literal one
  CHECK(F(QStringLiteral("a;;b"), QStringLiteral("/a;b")));
  CHECK(!F(QStringLiteral("a;;b"), QStringLiteral("/a")));
  // Second | is an error
  CHECK(!Valid(QStringLiteral("*.txt | *.bak | *.tmp")));
  // Unterminated set
  int ErrorStart = -1;
  CHECK(!Valid(QStringLiteral("*.txt; [abc"), &ErrorStart));
  CHECK(ErrorStart == 7);
  // Failed assignment keeps previous masks
  FileMasks M(QStringLiteral("*.txt"));
  try
  {
    M.SetMasks(QStringLiteral("[x"));
    CHECK(false);
  }
  catch (const FileMasksException & E)
  {
    CHECK(E.Message() == QStringLiteral("Mask is invalid near '[x'."));
  }
  CHECK(M.Masks() == QStringLiteral("*.txt"));
  CHECK(M.Matches(QStringLiteral("/a.txt"), false, false));
  CHECK(M.IncludeFileMasksStr() == QStringList{QStringLiteral("*.txt")});
}

static void TestDirectories()
{
  // Directory masks end with slash; they do not affect files
  CHECK(D(QStringLiteral("*/"), QStringLiteral("/home/dir")));
  CHECK(!D(QStringLiteral("| */"), QStringLiteral("/home/dir")));
  CHECK(!D(QStringLiteral("| .svn/; .git/"), QStringLiteral("/project/.git")));
  CHECK(D(QStringLiteral("| .svn/; .git/"), QStringLiteral("/project/src")));
  CHECK(F(QStringLiteral("| .svn/; .git/"), QStringLiteral("/project/.git")));   // file named .git is not excluded
  // Include directory mask: subdirectories of included directory are included too
  CHECK(D(QStringLiteral("backup/"), QStringLiteral("/data/backup")));
  CHECK(D(QStringLiteral("backup/"), QStringLiteral("/data/backup/2024")));
  CHECK(!D(QStringLiteral("backup/"), QStringLiteral("/data/other")));
  // ... while files are implicitly included when there is no file include mask
  CHECK(F(QStringLiteral("backup/"), QStringLiteral("/data/a.txt")));
  // File masks do not affect directories
  CHECK(D(QStringLiteral("*.txt"), QStringLiteral("/data/sub")));
  CHECK(D(QStringLiteral("| *.txt"), QStringLiteral("/data/x.txt")));
  // Mixed
  CHECK(F(QStringLiteral("*.txt; logs/"), QStringLiteral("/a/b.txt")));
  CHECK(!F(QStringLiteral("*.txt; logs/"), QStringLiteral("/a/b.doc")));
  CHECK(D(QStringLiteral("*.txt; logs/"), QStringLiteral("/a/logs")));
  CHECK(!D(QStringLiteral("*.txt; logs/"), QStringLiteral("/a/cache")));
  FileMasks M(QStringLiteral("*.txt; logs/ | tmp/"));
  CHECK(M.IncludeDirectoryMasksStr() == QStringList{QStringLiteral("logs")});
  CHECK(M.ExcludeDirectoryMasksStr() == QStringList{QStringLiteral("tmp")});
  CHECK(M.IncludeFileMasksStr() == QStringList{QStringLiteral("*.txt")});
}

static void TestPaths()
{
  // Path masks
  CHECK(F(QStringLiteral("/home/martin/*.txt"), QStringLiteral("/home/martin/a.txt")));
  CHECK(!F(QStringLiteral("/home/martin/*.txt"), QStringLiteral("/home/peter/a.txt")));
  CHECK(!F(QStringLiteral("/home/martin/*.txt"), QStringLiteral("/home/martin/sub/a.txt")));
  CHECK(F(QStringLiteral("/home/*/*.txt"), QStringLiteral("/home/martin/sub/a.txt")));   // '*' matches slashes too
  CHECK(F(QStringLiteral("/*.txt"), QStringLiteral("/a.txt")));
  CHECK(!F(QStringLiteral("| /home/martin/*"), QStringLiteral("/home/martin/a.txt")));
  CHECK(F(QStringLiteral("| /home/martin/*"), QStringLiteral("/home/peter/a.txt")));
  CHECK(!D(QStringLiteral("| /var/www/cache/"), QStringLiteral("/var/www/cache")));
  CHECK(D(QStringLiteral("| /var/www/cache/"), QStringLiteral("/var/cache")));
  // Name + path form
  FileMasks M(QStringLiteral("/home/martin/*.txt"));
  CHECK(M.Matches(QStringLiteral("a.txt"), false, QStringLiteral("/home/martin/"), nullptr));
  CHECK(!M.MatchesFileName(QStringLiteral("a.txt")));
}

static void TestSize()
{
  // Size conditions
  CHECK(F(QStringLiteral(">100K"), QStringLiteral("/a"), 100 * 1024 + 1));
  CHECK(!F(QStringLiteral(">100K"), QStringLiteral("/a"), 100 * 1024));
  CHECK(F(QStringLiteral(">=100K"), QStringLiteral("/a"), 100 * 1024));
  CHECK(F(QStringLiteral("<1M"), QStringLiteral("/a"), 1024 * 1024 - 1));
  CHECK(!F(QStringLiteral("<1M"), QStringLiteral("/a"), 1024 * 1024));
  CHECK(F(QStringLiteral("<=1G"), QStringLiteral("/a"), 1024LL * 1024 * 1024));
  CHECK(F(QStringLiteral("*.zip>1M<10M"), QStringLiteral("/a.zip"), 5 * 1024 * 1024));
  CHECK(!F(QStringLiteral("*.zip>1M<10M"), QStringLiteral("/a.zip"), 11 * 1024 * 1024));
  CHECK(!F(QStringLiteral("*.zip>1M<10M"), QStringLiteral("/a.rar"), 5 * 1024 * 1024));
  CHECK(F(QStringLiteral(">100"), QStringLiteral("/a"), 101));
  CHECK(!F(QStringLiteral("| >10M"), QStringLiteral("/a"), 20 * 1024 * 1024));
  CHECK(F(QStringLiteral("| >10M"), QStringLiteral("/a"), 20));
  // Condition without params does not match
  FileMasks M(QStringLiteral(">1K"));
  CHECK(!M.Matches(QStringLiteral("/a"), false, false, nullptr));
  // Invalid
  CHECK(!Valid(QStringLiteral(">10X")));
  CHECK(!Valid(QStringLiteral(">1K>2K")));
  CHECK(!Valid(QStringLiteral("dir/>1K")));   // no size condition for directories
}

static void TestTime()
{
  QDateTime Ref(QDate(2024, 6, 15), QTime(12, 0));
  CHECK(F(QStringLiteral(">2024-01-01"), QStringLiteral("/a"), 0, Ref));
  CHECK(!F(QStringLiteral("<2024-01-01"), QStringLiteral("/a"), 0, Ref));
  CHECK(F(QStringLiteral("<2024-06-15 12:00:01"), QStringLiteral("/a"), 0, Ref));
  CHECK(!F(QStringLiteral("<2024-06-15 12:00"), QStringLiteral("/a"), 0, Ref));
  CHECK(F(QStringLiteral("<=2024-06-15 12:00"), QStringLiteral("/a"), 0, Ref));
  CHECK(F(QStringLiteral("*.log>=2024-06-01<2024-07-01"), QStringLiteral("/x.log"), 0, Ref));
  CHECK(!F(QStringLiteral("*.log>=2024-06-01<2024-07-01"), QStringLiteral("/x.txt"), 0, Ref));
  // Relative: ">1D" modified in last 24 hours
  QDateTime Now = QDateTime::currentDateTime();
  CHECK(F(QStringLiteral(">1D"), QStringLiteral("/a"), 0, Now.addSecs(-3600)));
  CHECK(!F(QStringLiteral(">1D"), QStringLiteral("/a"), 0, Now.addSecs(-2 * 86400)));
  CHECK(F(QStringLiteral("<1D"), QStringLiteral("/a"), 0, Now.addSecs(-2 * 86400)));
  CHECK(F(QStringLiteral(">2H"), QStringLiteral("/a"), 0, Now.addSecs(-3600)));
  CHECK(!F(QStringLiteral(">30N"), QStringLiteral("/a"), 0, Now.addSecs(-3600)));
  CHECK(F(QStringLiteral(">60S"), QStringLiteral("/a"), 0, Now.addSecs(-10)));
  CHECK(F(QStringLiteral(">1Y"), QStringLiteral("/a"), 0, Now.addDays(-200)));
  CHECK(F(QStringLiteral(">=today"), QStringLiteral("/a"), 0, QDateTime(Now.date(), QTime(0, 0, 1))));
  CHECK(!F(QStringLiteral(">=today"), QStringLiteral("/a"), 0, QDateTime(Now.date().addDays(-1), QTime(23, 59))));
  CHECK(F(QStringLiteral(">=yesterday"), QStringLiteral("/a"), 0, QDateTime(Now.date().addDays(-1), QTime(0, 0, 1))));
  CHECK(F(QStringLiteral(">=1DS"), QStringLiteral("/a"), 0, QDateTime(Now.date().addDays(-1), QTime(0, 0, 1))));
  CHECK(!F(QStringLiteral(">=1DS"), QStringLiteral("/a"), 0, QDateTime(Now.date().addDays(-2), QTime(23, 59))));
  // Plain number is a size, not a time
  CHECK(F(QStringLiteral(">5"), QStringLiteral("/a"), 6, Ref));
  CHECK(!Valid(QStringLiteral(">2024-13-45")));
  CHECK(!Valid(QStringLiteral(">1D>2D")));
}

static void TestHelpers()
{
  CHECK(MaskFileName(QStringLiteral("readme.txt"), QStringLiteral("*.bak")) == QStringLiteral("readme.bak"));
  CHECK(MaskFileName(QStringLiteral("readme.txt"), QStringLiteral("*.*")) == QStringLiteral("readme.txt"));
  CHECK(MaskFileName(QStringLiteral("readme.txt"), QStringLiteral("new.*")) == QStringLiteral("new.txt"));
  CHECK(IsFileNameMask(QStringLiteral("*.*")));
  CHECK(!IsFileNameMask(QStringLiteral("name")));
  CHECK(!IsEffectiveFileNameMask(QStringLiteral("*.*")));
  CHECK(IsEffectiveFileNameMask(QStringLiteral("*.txt")));
  qint64 Size = 0;
  CHECK(TryStrToSize(QStringLiteral("100K"), Size) && (Size == 102400));
  CHECK(TryStrToSize(QStringLiteral("2 M"), Size) && (Size == 2 * 1048576));
  CHECK(!TryStrToSize(QStringLiteral("K"), Size));
}

static bool OpenCopyParamKey(IniStorage & Storage, bool CanCreate)
{
  return Storage.OpenSubKey(QStringLiteral("Configuration"), CanCreate) &&
    Storage.OpenSubKey(QStringLiteral("Interface"), CanCreate) &&
    Storage.OpenSubKey(QStringLiteral("CopyParam"), CanCreate);
}

static void TestCopyParam()
{
  CopyParam Defaults;
  CHECK(Defaults.Mode == CopyParam::tmBinary);
  CHECK(Defaults.ResumeSupport == CopyParam::rsSmart);
  CHECK(Defaults.ResumeThreshold == 100 * 1024);
  CHECK(Defaults.Rights == 0644);
  CHECK(Defaults.PreserveTime && !Defaults.PreserveRights && Defaults.AddXToDirectories);
  CHECK(Defaults.CPSLimit == 0);
  CHECK(Defaults.GetInfoStr(QStringLiteral("; "), 0) == QStringLiteral("Transfer type: Binary"));
  CHECK(Defaults.GetInfoStr(QStringLiteral("; "), cpaNoTransferMode) == QStringLiteral("Default transfer settings"));

  // Automatic transfer mode
  CopyParam P;
  P.Mode = CopyParam::tmAutomatic;
  CHECK(P.UseAsciiTransfer(QStringLiteral("/www/index.html"), false, FileMasks::Params()));
  CHECK(P.UseAsciiTransfer(QStringLiteral("/www/.htaccess"), false, FileMasks::Params()));
  CHECK(!P.UseAsciiTransfer(QStringLiteral("/www/logo.png"), false, FileMasks::Params()));
  CHECK(P.GetInfoStr(QStringLiteral("; "), 0) == QStringLiteral("Transfer type: Automatic"));
  P.AsciiFileMask = QStringLiteral("*.txt");
  CHECK(P.GetInfoStr(QStringLiteral("; "), 0) == QStringLiteral("Transfer type: Automatic (*.txt)"));

  // Info string lists non-default settings only
  P = CopyParam();
  P.PreserveTime = false;
  P.PreserveRights = true;
  P.Rights = 0755;
  P.CPSLimit = 512 * 1024;
  P.NewerOnly = true;
  P.ExcludeHiddenFiles = true;
  P.IncludeFileMask = QStringLiteral("*.txt");
  P.FileNameCase = CopyParam::ncLowerCase;
  QString Info = P.GetInfoStr(QStringLiteral("; "), 0);
  CHECK(Info == QStringLiteral("Transfer type: Binary; Filename modification: Lower case; Set permissions: rwxr-xr-x, Add X to directories; "
    "Do not preserve timestamp; File mask: *.txt; Transfer speed limit: 512 KB/s; New and updated files only; Exclude hidden files"));
  // Download context: permissions are not applicable
  CHECK(P.GetInfoStr(QStringLiteral("; "), CopyParam::UsableAttrs(false)).endsWith(
    QStringLiteral("Some configured transfer settings cannot be used in current context")));
  P = CopyParam();
  P.PreserveTimeDirs = true;
  CHECK(P.GetInfoStr(QStringLiteral("; "), 0) == QStringLiteral("Transfer type: Binary; Preserve timestamp (including directories)"));
  P = CopyParam();
  P.ResumeSupport = CopyParam::rsOff;
  CHECK(P.GetInfoStr(QStringLiteral("; "), 0) == QStringLiteral("Transfer type: Binary"));

  // Resume
  CHECK(!Defaults.AllowResume(100 * 1024 - 1, QStringLiteral("a")));
  CHECK(Defaults.AllowResume(100 * 1024, QStringLiteral("a")));
  P.ResumeSupport = CopyParam::rsOn;
  CHECK(P.AllowResume(0, QStringLiteral("a")));
  CHECK(!P.AllowResume(0, QString(250, QLatin1Char('a'))));

  // AllowTransfer
  P = CopyParam();
  P.ExcludeHiddenFiles = true;
  CHECK(!P.AllowTransfer(QStringLiteral("/a/.x"), false, false, FileMasks::Params(), true));
  P.IncludeFileMask = QStringLiteral("*.txt");
  CHECK(P.AllowTransfer(QStringLiteral("/a/b.txt"), false, false, FileMasks::Params(), false));
  CHECK(!P.AllowTransfer(QStringLiteral("/a/b.doc"), false, false, FileMasks::Params(), false));
  CHECK(!P.AllowAnyTransfer());
  CHECK(CopyParam().AllowAnyTransfer());

  // File name case
  P = CopyParam();
  P.FileNameCase = CopyParam::ncUpperCase;
  CHECK(P.ChangeFileName(QStringLiteral("Read.me")) == QStringLiteral("READ.ME"));
  P.FileNameCase = CopyParam::ncFirstUpperCase;
  CHECK(P.ChangeFileName(QStringLiteral("rEAD.ME")) == QStringLiteral("Read.me"));
  P.FileNameCase = CopyParam::ncLowerCaseShort;
  CHECK(P.ChangeFileName(QStringLiteral("README.TXT")) == QStringLiteral("readme.txt"));
  CHECK(P.ChangeFileName(QStringLiteral("LONGFILENAME.TXT")) == QStringLiteral("LONGFILENAME.TXT"));
  CHECK(P.ChangeFileName(QStringLiteral("Readme.txt")) == QStringLiteral("Readme.txt"));

  // Speed limit text
  CHECK(SetSpeedLimit(0) == QStringLiteral("Unlimited"));
  CHECK(SetSpeedLimit(256 * 1024) == QStringLiteral("256"));
  unsigned long Speed = 1;
  CHECK(TryGetSpeedLimit(QStringLiteral("unlimited"), Speed) && (Speed == 0));
  CHECK(TryGetSpeedLimit(QStringLiteral("64"), Speed) && (Speed == 64 * 1024));
  CHECK(!TryGetSpeedLimit(QStringLiteral("fast"), Speed));

  // Load/Save with WinSCP key names
  QTemporaryDir Dir;
  QString IniPath = Dir.filePath(QStringLiteral("t.ini"));
  {
    IniFile Ini;
    IniStorage Storage(&Ini);
    CHECK(OpenCopyParamKey(Storage, true));
    CopyParam S;
    S.Mode = CopyParam::tmAutomatic;
    S.Rights = 0600;
    S.PreserveRights = true;
    S.IncludeFileMask = QStringLiteral("*.txt | *.bak");
    S.CPSLimit = 1024 * 100;
    S.ResumeSupport = CopyParam::rsOff;
    S.ResumeThreshold = 5 * 1024 * 1024;
    S.ExcludeEmptyDirectories = true;
    S.FileNameCase = CopyParam::ncUpperCase;
    S.Queue = true;
    S.Save(Storage);
    CHECK(Ini.ReadRaw(QStringLiteral("Configuration\\Interface\\CopyParam"), QStringLiteral("Text")) == QStringLiteral("rw-------"));
    CHECK(Ini.ReadRaw(QStringLiteral("Configuration\\Interface\\CopyParam"), QStringLiteral("TransferMode")) == QStringLiteral("2"));
    CHECK(Ini.ReadRaw(QStringLiteral("Configuration\\Interface\\CopyParam"), QStringLiteral("CPSLimit")) == QStringLiteral("102400"));
    CHECK(Ini.ReadRaw(QStringLiteral("Configuration\\Interface\\CopyParam"), QStringLiteral("ResumeSupport")) == QStringLiteral("2"));
    CHECK(Ini.Save(IniPath));
  }
  {
    IniFile Ini;
    CHECK(Ini.Load(IniPath));
    IniStorage Storage(&Ini);
    CHECK(OpenCopyParamKey(Storage, false));
    CopyParam L;
    L.Load(Storage);
    CHECK(L.Mode == CopyParam::tmAutomatic);
    CHECK(L.Rights == 0600);
    CHECK(L.PreserveRights);
    CHECK(L.IncludeFileMask.Masks() == QStringLiteral("*.txt | *.bak"));
    CHECK(L.CPSLimit == 1024 * 100);
    CHECK(L.ResumeSupport == CopyParam::rsOff);
    CHECK(L.ResumeThreshold == 5 * 1024 * 1024);
    CHECK(L.ExcludeEmptyDirectories);
    CHECK(L.FileNameCase == CopyParam::ncUpperCase);
    CHECK(L.Queue);
    CHECK(L.AsciiFileMask == CopyParam().AsciiFileMask);
  }
  {
    // Obsolete ExcludeFileMask is converted
    IniFile Ini;
    Ini.WriteRaw(QStringLiteral("CP"), QStringLiteral("ExcludeFileMask"), QStringLiteral("*.bak"));
    IniStorage Storage(&Ini);
    CHECK(Storage.OpenSubKey(QStringLiteral("CP"), false));
    CopyParam L;
    L.Load(Storage);
    CHECK(L.IncludeFileMask.Masks() == QStringLiteral("|*.bak"));
  }
}

int main()
{
  TestWildcards();
  TestLists();
  TestDirectories();
  TestPaths();
  TestSize();
  TestTime();
  TestHelpers();
  TestCopyParam();
  if (Failures == 0)
  {
    std::printf("test_filemasks: all tests passed\n");
    return 0;
  }
  std::printf("test_filemasks: %d failure(s)\n", Failures);
  return 1;
}
