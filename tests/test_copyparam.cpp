// Transfer settings tests (WP10): core/CopyParam (target file mask of the Copy
// dialog, PreserveReadOnly, usable attributes per protocol, text mode line
// ending conversion, timestamp precision of "New and updated files only") and
// core/CopyParamList (transfer settings presets, WinSCP.ini compatibility,
// autoselection rules, CopyParamPresets model).
#include "core/Configuration.h"
#include "core/CopyParam.h"
#include "core/CopyParamList.h"
#include "core/FileSystem.h"
#include "core/IniStorage.h"
#include "core/Terminal.h"

#include <QCoreApplication>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cstdio>

static int Failures = 0;
static int Checks = 0;

#define CHECK(Cond) \
  do { Checks++; if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_EQ(A, B) \
  do { Checks++; auto VA = (A); auto VB = (B); if (!(VA == VB)) { std::printf("FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #A, #B); \
    std::fflush(stdout); Failures++; } } while (0)

static void TestFileMask()
{
  CopyParam P;
  CHECK_EQ(P.FileMask, QStringLiteral("*.*"));
  CHECK_EQ(P.ChangeFileName(QStringLiteral("a.txt"), true), QStringLiteral("a.txt"));
  P.FileMask = QStringLiteral("b.txt");
  CHECK_EQ(P.ChangeFileName(QStringLiteral("a.txt"), true), QStringLiteral("b.txt"));
  // nested files keep their names
  CHECK_EQ(P.ChangeFileName(QStringLiteral("a.txt"), false), QStringLiteral("a.txt"));
  CHECK_EQ(P.ChangeFileName(QStringLiteral("a.txt")), QStringLiteral("a.txt"));
  P.FileMask = QStringLiteral("*.bak");
  CHECK_EQ(P.ChangeFileName(QStringLiteral("readme.txt"), true), QStringLiteral("readme.bak"));
  P.FileMask = QString();   // target ended with a slash
  CHECK_EQ(P.ChangeFileName(QStringLiteral("readme.txt"), true), QStringLiteral("readme.txt"));
  P.FileMask = QStringLiteral("*.bak");
  P.FileNameCase = CopyParam::ncUpperCase;
  CHECK_EQ(P.ChangeFileName(QStringLiteral("readme.txt"), true), QStringLiteral("README.BAK"));
  // FileMask is not a part of the settings identity (as in WinSCP)
  CopyParam A;
  CopyParam B;
  B.FileMask = QStringLiteral("x");
  CHECK(A == B);
  CHECK(!IsFileNameMask(QStringLiteral("b.txt")));
  CHECK(IsFileNameMask(QStringLiteral("*.bak")));
  QString Message = FormatMultiFilesToOneConfirmation(QStringLiteral("/srv/b.txt"));
  CHECK(Message.contains(QStringLiteral("single file 'b.txt' in a directory '/srv'")));
  CHECK(Message.contains(QStringLiteral("directory '/srv/b.txt/'")));
}

// TCopyParamType::ValidLocalFileName / RestoreChars (Windows only)
static void TestLocalFileNames()
{
  const QStringList Names = {
    QStringLiteral("report 12:30.txt"), QStringLiteral("a?b*c|d<e>f\"g\\h"), QStringLiteral("dot."), QStringLiteral("space "),
    QStringLiteral("con"), QStringLiteral("NUL.txt"), QStringLiteral("100%"), QStringLiteral("x%3Ay"), QStringLiteral("x%00y"),
    QStringLiteral("plain.txt"), QStringLiteral("tab\there")};
#ifdef _WIN32
  CHECK_EQ(CopyParam::ValidLocalFileName(QStringLiteral("report 12:30.txt")), QStringLiteral("report 12%3A30.txt"));
  CHECK_EQ(CopyParam::ValidLocalFileName(QStringLiteral("a?b|c")), QStringLiteral("a%3Fb%7Cc"));
  CHECK_EQ(CopyParam::ValidLocalFileName(QStringLiteral("dot.")), QStringLiteral("dot%2E"));
  CHECK_EQ(CopyParam::ValidLocalFileName(QStringLiteral("con")), QStringLiteral("con%00"));
  CHECK_EQ(CopyParam::ValidLocalFileName(QStringLiteral("NUL.txt")), QStringLiteral("NUL%00.txt"));
  CHECK_EQ(CopyParam::ValidLocalFileName(QStringLiteral("100%")), QStringLiteral("100%"));
  // a literal token is protected, so that it comes back unchanged
  CHECK_EQ(CopyParam::ValidLocalFileName(QStringLiteral("x%3Ay")), QStringLiteral("x%253Ay"));
  CHECK_EQ(CopyParam::ValidLocalFileName(QStringLiteral("plain.txt")), QStringLiteral("plain.txt"));
#endif
  for (const QString & Name : Names)
  {
    CHECK_EQ(CopyParam::RestoreChars(CopyParam::ValidLocalFileName(Name)), Name);
  }
}

static void TestPreserveReadOnly()
{
  CopyParam P;
  CHECK(!P.PreserveReadOnly);
  CHECK_EQ(PreserveReadOnlyMode(0644, 0444, true), 0444u);
  CHECK_EQ(PreserveReadOnlyMode(0664, 0555, true), 0444u);
  CHECK_EQ(PreserveReadOnlyMode(0644, 0644, true), 0644u);
  CHECK_EQ(PreserveReadOnlyMode(0644, 0444, false), 0644u);
  P.PreserveReadOnly = true;
  CHECK(P.GetInfoStr(QStringLiteral("; "), CopyParam::UsableAttrs(false)).contains(QStringLiteral("Preserve read-only")));
  // not applicable to uploads
  CHECK(!P.GetInfoStr(QStringLiteral("; "), CopyParam::UsableAttrs(true)).contains(QStringLiteral("Preserve read-only")));
  CopyParam Q;
  CHECK(P != Q);
}

static int Attrs(TFSProtocol Protocol, bool ToRemote)
{
  return CopyParam::UsableAttrs(ToRemote, [Protocol](int Capability)
    { return Terminal::ProtocolCapability(Protocol, static_cast<FSCapability>(Capability)); });
}

static void TestUsableAttrs()
{
  // SFTP: the old direction-only answer is the same
  CHECK_EQ(Attrs(fsSFTP, true), CopyParam::UsableAttrs(true));
  CHECK_EQ(Attrs(fsSFTP, false), CopyParam::UsableAttrs(false));
  CHECK_EQ(CopyParam::UsableAttrs(true, static_cast<const Terminal *>(nullptr)), CopyParam::UsableAttrs(true));
  // download never sets permissions
  CHECK((Attrs(fsSFTP, false) & cpaNoRights) != 0);
  CHECK((Attrs(fsSFTP, true) & cpaNoRights) == 0);
  CHECK((Attrs(fsSFTP, false) & cpaNoPreserveReadOnly) == 0);
  CHECK((Attrs(fsSFTP, true) & cpaNoPreserveReadOnly) != 0);
  // S3 cannot change permissions
  CHECK((Attrs(fsS3, true) & cpaNoRights) != 0);
  CHECK((Attrs(fsS3, false) & cpaNoPreserveReadOnly) != 0);
  // the per-protocol answer differs from SFTP somewhere for every other protocol
  for (TFSProtocol P : {fsSCPonly, fsFTP, fsWebDAV, fsS3})
  {
    bool Differs = (Attrs(P, true) != Attrs(fsSFTP, true)) || (Attrs(P, false) != Attrs(fsSFTP, false));
    CHECK(Differs);
  }
  // Move: cpaNoClearArchive
  CHECK((CopyParam::UsableAttrs(true, static_cast<const Terminal *>(nullptr), true) & cpaNoClearArchive) != 0);
}

static QByteArray ConvertChunks(TextEOLConverter & Converter, const QList<QByteArray> & Chunks)
{
  QByteArray Result;
  for (const QByteArray & Chunk : Chunks)
  {
    Result += Converter.Convert(Chunk);
  }
  Result += Converter.Finish();
  return Result;
}

static void TestEOL()
{
  CHECK_EQ(EOLToStr(0), QByteArray("\n"));
  CHECK_EQ(EOLToStr(1), QByteArray("\r\n"));
  CHECK_EQ(EOLToStr(2), QByteArray("\r"));
  {
    // download from a CRLF server: CRLF -> LF, also when split between chunks
    TextEOLConverter C(EOLToStr(1), LocalEOL);
    CHECK(!C.Identity());
    CHECK_EQ(ConvertChunks(C, {QByteArray("a\r\nb\r"), QByteArray("\nc\r"), QByteArray("x\r")}), QByteArray("a\nb\nc\rx\r"));
  }
  {
    TextEOLConverter C(EOLToStr(1), LocalEOL);
    QByteArray Out;
    for (char Ch : QByteArray("l1\r\nl2\r\n\r\nl4"))
    {
      Out += C.Convert(QByteArray(1, Ch));
    }
    Out += C.Finish();
    CHECK_EQ(Out, QByteArray("l1\nl2\n\nl4"));
  }
  {
    // upload to a CRLF server: LF -> CRLF, existing CRLF passes unmodified
    TextEOLConverter C(LocalEOL, EOLToStr(1));
    CHECK_EQ(ConvertChunks(C, {QByteArray("a\nb\r"), QByteArray("\nc\n")}), QByteArray("a\r\nb\r\nc\r\n"));
  }
  {
    TextEOLConverter C(LocalEOL, EOLToStr(1));
    CHECK_EQ(ConvertChunks(C, {QByteArray("a\r"), QByteArray("b")}), QByteArray("a\rb"));
  }
  {
    // CR (old Mac) server
    TextEOLConverter C(EOLToStr(2), LocalEOL);
    CHECK_EQ(ConvertChunks(C, {QByteArray("a\rb\r")}), QByteArray("a\nb\n"));
  }
  {
    TextEOLConverter C(LocalEOL, LocalEOL);
    CHECK(C.Identity());
    CHECK_EQ(ConvertChunks(C, {QByteArray("a\r\nb\n")}), QByteArray("a\r\nb\n"));
  }
}

static void TestPrecision()
{
  QDateTime Local(QDate(2024, 5, 1), QTime(10, 20, 35), QTimeZone::UTC);
  QDateTime FtpRemote(QDate(2024, 5, 1), QTime(10, 20, 0), QTimeZone::UTC);
  // with minute precision the local file is not newer
  CHECK(!IsNewerModification(Local, mfFull, FtpRemote, mfMDHM));
  CHECK(IsNewerModification(Local, mfFull, FtpRemote, mfFull));
  CHECK(IsNewerModification(Local.addSecs(60), mfFull, FtpRemote, mfYMDHM));
  // milliseconds are ignored with full precision
  QDateTime A(QDate(2024, 5, 1), QTime(10, 20, 35, 900), QTimeZone::UTC);
  CHECK(!IsNewerModification(A, mfFull, Local, mfFull));
  CHECK_EQ(LessDateTimePrecision(mfFull, mfMDY), mfMDY);
  CHECK_EQ(ReduceDateTimePrecision(Local, mfMDY).time(), QTime(0, 0));
}

static void TestCopyParamList()
{
  CopyParam Default;
  CopyParamList Defaults = CopyParamList::Defaults(Default);
  CHECK_EQ(Defaults.Count(), 4);
  CHECK_EQ(Defaults.Name(0), QStringLiteral("&Text"));
  CHECK_EQ(Defaults.Param(0).Mode, CopyParam::tmAscii);
  CHECK_EQ(Defaults.Name(1), QStringLiteral("&Binary"));
  CHECK_EQ(Defaults.Param(1).Mode, CopyParam::tmBinary);
  CHECK_EQ(Defaults.Name(2), QStringLiteral("&New and updated files only"));
  CHECK(Defaults.Param(2).NewerOnly);
  CHECK_EQ(Defaults.Name(3), QStringLiteral("Exclude &directories"));
  CHECK_EQ(Defaults.Param(3).IncludeFileMask.Masks(), QStringLiteral("| */"));
  CHECK(!Defaults.Modified());
  CHECK(!Defaults.AnyRule());

  CopyParamList List = Defaults;
  CopyParam Web;
  Web.PreserveRights = true;
  Web.Rights = 0640;
  CopyParamRuleData RuleData;
  RuleData.HostName = QStringLiteral("*.example.com");
  RuleData.RemoteDirectory = QStringLiteral("/var/www/*");
  List.Add(QStringLiteral("Web"), Web, CopyParamRule(RuleData));
  CHECK(List.Modified());
  CHECK(List.AnyRule());
  CHECK_EQ(List.IndexOfName(QStringLiteral("web")), 4);
  CHECK(List != Defaults);

  CopyParamRuleData Session;
  Session.HostName = QStringLiteral("www.example.com");
  Session.UserName = QStringLiteral("martin");
  Session.RemoteDirectory = QStringLiteral("/var/www/html");
  CHECK_EQ(List.Find(Session), 4);
  Session.RemoteDirectory = QStringLiteral("/home/martin");
  CHECK_EQ(List.Find(Session), -1);
  Session.RemoteDirectory = QStringLiteral("/var/www/html");
  Session.HostName = QStringLiteral("example.org");
  CHECK_EQ(List.Find(Session), -1);
  CHECK(List.Rule(4)->GetInfoStr(QStringLiteral("; ")) ==
    QStringLiteral("Hostname: *.example.com; Remote directory: /var/www/*"));
  CHECK(CopyParamRule().IsEmpty());

  List.Move(4, 0);
  CHECK_EQ(List.Name(0), QStringLiteral("Web"));
  List.Change(0, QStringLiteral("Web2"), Web);
  CHECK(List.Rule(0) == nullptr);
  List.Delete(0);
  CHECK(List == Defaults);

  bool Thrown = false;
  try
  {
    CopyParamList::ValidateName(QStringLiteral("a/b"));
  }
  catch (const ExtException & E)
  {
    Thrown = E.Message().contains(QStringLiteral("cannot contain"));
  }
  CHECK(Thrown);
  Thrown = false;
  try
  {
    CopyParamList::ValidateName(QStringLiteral("Good name"));
  }
  catch (const ExtException &)
  {
    Thrown = true;
  }
  CHECK(!Thrown);
}

// Presets as WinSCP writes them (incl. values NixSCP does not use)
static const char * WinSCPIni =
  "[Configuration\\Interface\\CopyParam]\r\n"
  "TransferMode=2\r\n"
  "ResumeSupport=2\r\n"
  "CopyParamList=2\r\n"
  "[Configuration\\Interface\\CopyParam\\0]\r\n"
  "Name=Web%20upload\r\n"
  "TransferMode=0\r\n"
  "PreserveRights=1\r\n"
  "Text=rw-r-----\r\n"
  "ClearArchive=1\r\n"
  "ReplaceInvalidChars=0\r\n"
  "EncryptNewFiles=0\r\n"
  "HasRule=1\r\n"
  "HostName=*.example.com\r\n"
  "UserName=\r\n"
  "RemoteDirectory=/var/www/*\r\n"
  "LocalDirectory=\r\n"
  "[Configuration\\Interface\\CopyParam\\1]\r\n"
  "Name=&Text\r\n"
  "TransferMode=1\r\n"
  "PreserveReadOnly=1\r\n"
  "HasRule=0\r\n";

static void TestIni()
{
  IniFile File;
  CHECK(File.LoadFromData(QByteArray(WinSCPIni)));
  IniStorage Storage(&File);
  CHECK(Storage.OpenSubKeyPath(QStringLiteral("Configuration\\Interface\\CopyParam"), false));
  CopyParamList List;
  List.Load(Storage, Storage.ReadInteger(QStringLiteral("CopyParamList"), -1));
  CHECK_EQ(List.Count(), 2);
  CHECK_EQ(List.Name(0), QStringLiteral("Web upload"));
  CHECK(List.Param(0).PreserveRights);
  CHECK_EQ(List.Param(0).Rights, 0640u);
  CHECK(List.Rule(0) != nullptr);
  CHECK_EQ(List.Rule(0)->Data.HostName, QStringLiteral("*.example.com"));
  CHECK(List.Param(1).PreserveReadOnly);
  CHECK(List.Rule(1) == nullptr);

  // save into a fresh file: unknown values survive
  IniFile Out;
  IniStorage OutStorage(&Out);
  CHECK(OutStorage.OpenSubKeyPath(QStringLiteral("Configuration\\Interface\\CopyParam"), true));
  OutStorage.WriteInteger(QStringLiteral("CopyParamList"), List.Count());
  List.Save(OutStorage);
  OutStorage.CloseSubKeyPath();
  QString Section0 = QStringLiteral("Configuration\\Interface\\CopyParam\\0");
  CHECK_EQ(Out.ReadRaw(Section0, QStringLiteral("ClearArchive")), QStringLiteral("1"));
  CHECK_EQ(Out.ReadRaw(Section0, QStringLiteral("ReplaceInvalidChars")), QStringLiteral("0"));
  CHECK_EQ(Out.ReadRaw(Section0, QStringLiteral("EncryptNewFiles")), QStringLiteral("0"));
  CHECK_EQ(Out.ReadRaw(Section0, QStringLiteral("Name")), QStringLiteral("Web%20upload"));
  CHECK_EQ(Out.ReadRaw(Section0, QStringLiteral("HasRule")), QStringLiteral("1"));
  CHECK_EQ(Out.ReadRaw(Section0, QStringLiteral("Text")), QStringLiteral("rw-r-----"));
  CHECK(Out.ReadRaw(Section0, QStringLiteral("Queue")).isEmpty());
  CHECK_EQ(Out.ReadRaw(QStringLiteral("Configuration\\Interface\\CopyParam\\1"), QStringLiteral("PreserveReadOnly")), QStringLiteral("1"));

  // reload equals
  IniStorage Again(&Out);
  CHECK(Again.OpenSubKeyPath(QStringLiteral("Configuration\\Interface\\CopyParam"), false));
  CopyParamList Reloaded;
  Reloaded.Load(Again, 2);
  CHECK(Reloaded == List);

  // a shorter list removes the stale subkeys
  Reloaded.Delete(1);
  Reloaded.Save(Again);
  CHECK(!Out.SectionExists(QStringLiteral("Configuration\\Interface\\CopyParam\\1")));
}

static void TestPresets(const QString & Dir)
{
  QString IniPath = Dir + QStringLiteral("/presets.ini");
  {
    QFile F(IniPath);
    CHECK(F.open(QIODevice::WriteOnly));
    F.write(
      "[Configuration\\Interface\\CopyParam]\r\n"
      "ResumeSupport=2\r\n"
      "Queue=1\r\n");
  }
  Configuration & C = Configuration::Instance();
  C.SetIniFileName(IniPath);
  C.Load();
  CopyParamPresets & Presets = CopyParamPresets::Instance();
  Presets.Reload();
  CHECK(Presets.IsDefaultList());
  CHECK_EQ(Presets.List().Count(), 4);
  CHECK(Presets.Current().isEmpty());
  CHECK_EQ(Presets.CurrentIndex(), -1);
  CHECK(Presets.HasPreset(QString()));
  CHECK(Presets.HasPreset(QStringLiteral("&Binary")));
  CHECK(!Presets.HasPreset(QStringLiteral("Nope")));
  CHECK(Presets.CurrentCopyParam() == C.DefaultCopyParam());

  // a preset keeps the resume and queue options of the default
  CopyParam Text = Presets.Preset(QStringLiteral("&Text"));
  CHECK_EQ(Text.Mode, CopyParam::tmAscii);
  CHECK_EQ(Text.ResumeSupport, CopyParam::rsOff);
  CHECK(Text.Queue);

  int ChangedSignals = 0;
  QObject::connect(&Presets, &CopyParamPresets::ListChanged, [&ChangedSignals]() { ChangedSignals++; });

  // unchanged list stays built-in (CopyParamList not written)
  Presets.SetList(Presets.List());
  CHECK(Presets.IsDefaultList());
  CHECK_EQ(ChangedSignals, 0);

  CopyParamList List = Presets.List();
  CopyParam Web;
  Web.PreserveRights = true;
  CopyParamRuleData RuleData;
  RuleData.HostName = QStringLiteral("*.example.com");
  List.Add(QStringLiteral("Web"), Web, CopyParamRule(RuleData));
  Presets.SetList(List);
  CHECK(!Presets.IsDefaultList());
  CHECK_EQ(ChangedSignals, 1);
  CHECK(C.Save());

  // WinSCP.ini contents
  IniFile Saved;
  CHECK(Saved.Load(IniPath));
  CHECK_EQ(Saved.ReadRaw(QStringLiteral("Configuration\\Interface\\CopyParam"), QStringLiteral("CopyParamList")), QStringLiteral("5"));
  CHECK_EQ(Saved.ReadRaw(QStringLiteral("Configuration\\Interface\\CopyParam\\4"), QStringLiteral("Name")), QStringLiteral("Web"));
  CHECK_EQ(Saved.ReadRaw(QStringLiteral("Configuration\\Interface\\CopyParam\\0"), QStringLiteral("Name")), QStringLiteral("&Text"));

  // autoselection (TransferPresetAutoSelect)
  Presets.SelectByUser(QStringLiteral("&Binary"));
  CHECK_EQ(Presets.Current(), QStringLiteral("&Binary"));
  CopyParamRuleData Session;
  Session.HostName = QStringLiteral("ftp.example.com");
  Session.RemoteDirectory = QStringLiteral("/home");
  QString Message;
  QString Info;
  CHECK(Presets.AutoSelect(Session, 0, &Message, &Info));
  CHECK_EQ(Presets.Current(), QStringLiteral("Web"));
  CHECK_EQ(Message, QStringLiteral("Transfer settings preset 'Web' was automatically selected."));
  CHECK(Info.contains(QStringLiteral("Autoselection rule:\nHostname: *.example.com")));
  // same preset again: no change
  CHECK(!Presets.AutoSelect(Session, 0, &Message));
  // no rule matches: back to the preset selected by the user
  Session.HostName = QStringLiteral("other.org");
  CHECK(Presets.AutoSelect(Session, 0, &Message));
  CHECK_EQ(Presets.Current(), QStringLiteral("&Binary"));
  CHECK_EQ(Message, QStringLiteral("Returned back to transfer settings preset 'Binary'."));

  // removing the current preset resets the selection
  List = Presets.List();
  List.Delete(List.IndexOfName(QStringLiteral("&Binary")));
  Presets.SetList(List);
  CHECK(Presets.Current().isEmpty());
  CHECK(C.Save());

  // the list is read again from the file
  C.Load();
  Presets.Reload();
  CHECK_EQ(Presets.List().Count(), 4);
  CHECK(Presets.List().IndexOfName(QStringLiteral("Web")) >= 0);
}

int main(int argc, char ** argv)
{
  QCoreApplication App(argc, argv);
  QStandardPaths::setTestModeEnabled(true);
  QTemporaryDir Scratch;
  qputenv("XDG_CONFIG_HOME", Scratch.path().toUtf8());

  TestFileMask();
  TestLocalFileNames();
  TestPreserveReadOnly();
  TestUsableAttrs();
  TestEOL();
  TestPrecision();
  TestCopyParamList();
  TestIni();
  TestPresets(Scratch.path());

  std::printf("%s: %d checks, %d failures\n", (Failures == 0) ? "PASS" : "FAIL", Checks, Failures);
  return (Failures == 0) ? 0 : 1;
}
