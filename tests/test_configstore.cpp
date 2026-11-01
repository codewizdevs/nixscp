// Configuration storage tests (WP13): IniStorage::OpenSubKeyPath, root values,
// change sets; several instances sharing one nixscp.ini (core/Configuration.cpp,
// a second instance is this binary started as a child process); file watching;
// machine-wide defaults and policies; Export/Import; custom INI file;
// master password (byte-compatible with WinSCP's core/Cryptography.cpp).
#include "core/Configuration.h"
#include "core/IniStorage.h"
#include "core/Security.h"
#include "core/SessionData.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>
#include <memory>

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); Failures++; } } while (0)

static void WriteFile(const QString & Path, const QByteArray & Data)
{
  QFile F(Path);
  CHECK(F.open(QIODevice::WriteOnly | QIODevice::Truncate));
  F.write(Data);
}

static QByteArray ReadFile(const QString & Path)
{
  QFile F(Path);
  return F.open(QIODevice::ReadOnly) ? F.readAll() : QByteArray();
}

static IniFile LoadIni(const QString & Path)
{
  IniFile File;
  File.Load(Path);
  return File;
}

// Known-answer vectors computed by an independent implementation of
// WinSCP's AES256EncyptWithMAC/ScramblePassword/AES256CreateVerifier
// (Python, hashlib.pbkdf2_hmac + AES-ECB based CTR + hmac-sha1)
static const char * const KnownMasterPassword = "Master#Pass1";
static const char * const KnownVerifier =
  "202122232425262728292A2B2C2D2E2F404142434445464748494A4B4C4D4E4F3254A79CC98CC73B167B";
static const char * const KnownExternal =
  "A35D0102030405060708090A0B0C0D0E0F106CB938325772778F9218DDC675DA13F8E04026DD830B69EA3577CCAA47A1CDE4A8B93D1915323863654A55D1";
static const char * const KnownExternalLong =
  "A35D0102030405060708090A0B0C0D0E0F106CB9383257FDBEC1486A082FCC2A284BEEC8BA2C8849FC4FC84DAC0F49F208CD2D7F0B8435110A1213F3284395377974CD9DE14C0BF1AAF430FFB5ED42B87EA37BF8290ACB88C0CC5018B875DB61";

//---------------------------------------------------------------------------
static void TestIniStorage()
{
  IniFile File;
  CHECK(File.LoadFromData(
    "Policy=1\r\n"
    "\r\n"
    "[Configuration\\Interface]\r\n"
    "X=1\r\n"
    "\r\n"
    "[Sessions\\a%5Cb]\r\n"
    "HostName=h\r\n"));
  CHECK(File.ReadRaw(QString(), QStringLiteral("Policy")) == QStringLiteral("1"));
  CHECK(File.SaveToData().startsWith("Policy=1\r\n"));

  IniStorage Storage(&File);
  // the root key holds the policies
  CHECK(Storage.ReadBool(QStringLiteral("Policy"), false));
  CHECK(Storage.OpenSubKeyPath(QStringLiteral("Configuration\\Interface"), false));
  CHECK(Storage.CurrentSection() == QStringLiteral("Configuration\\Interface"));
  CHECK(Storage.ReadInteger(QStringLiteral("X"), 0) == 1);
  Storage.CloseSubKeyPath();
  CHECK(Storage.CurrentSection().isEmpty());
  // OpenSubKey munges the backslash (a site name may contain it)
  CHECK(!Storage.OpenSubKey(QStringLiteral("Configuration\\Interface"), false));
  CHECK(Storage.OpenSubKey(QStringLiteral("Sessions"), false));
  CHECK(Storage.OpenSubKey(QStringLiteral("a\\b"), false));
  CHECK(Storage.ReadString(QStringLiteral("HostName"), QString()) == QStringLiteral("h"));
  Storage.CloseSubKey();
  Storage.CloseSubKey();
  // all or nothing
  CHECK(!Storage.OpenSubKeyPath(QStringLiteral("Configuration\\Missing"), false));
  CHECK(Storage.CurrentSection().isEmpty());
  // nesting: CloseSubKey after OpenSubKeyPath closes the whole path
  CHECK(Storage.OpenSubKeyPath(QStringLiteral("A\\B"), true));
  CHECK(Storage.OpenSubKey(QStringLiteral("C"), true));
  CHECK(Storage.CurrentSection() == QStringLiteral("A\\B\\C"));
  Storage.CloseSubKey();
  CHECK(Storage.CurrentSection() == QStringLiteral("A\\B"));
  Storage.CloseSubKey();
  CHECK(Storage.CurrentSection().isEmpty());

  // copies are independent (implicit sharing detaches)
  IniFile Copy = File;
  Copy.WriteRaw(QStringLiteral("Configuration\\Interface"), QStringLiteral("X"), QStringLiteral("2"));
  CHECK(File.ReadRaw(QStringLiteral("Configuration\\Interface"), QStringLiteral("X")) == QStringLiteral("1"));

  // change sets
  IniFile From;
  From.LoadFromData("[S]\r\na=1\r\nb=2\r\n[T]\r\nx=1\r\n");
  IniFile To;
  To.LoadFromData("[S]\r\na=1\r\nb=3\r\nc=4\r\n[U]\r\ny=5\r\n[Empty]\r\n");
  IniFile Third;
  Third.LoadFromData("[S]\r\na=1\r\nb=2\r\nz=9\r\n[T]\r\nx=1\r\n[V]\r\nw=1\r\n");
  Third.Apply(IniFile::Diff(From, To));
  CHECK(Third.ReadRaw(QStringLiteral("S"), QStringLiteral("b")) == QStringLiteral("3"));
  CHECK(Third.ReadRaw(QStringLiteral("S"), QStringLiteral("c")) == QStringLiteral("4"));
  CHECK(Third.ReadRaw(QStringLiteral("S"), QStringLiteral("z")) == QStringLiteral("9"));
  CHECK(!Third.SectionExists(QStringLiteral("T")));
  CHECK(Third.ReadRaw(QStringLiteral("U"), QStringLiteral("y")) == QStringLiteral("5"));
  CHECK(Third.SectionExists(QStringLiteral("Empty")));
  CHECK(Third.ReadRaw(QStringLiteral("V"), QStringLiteral("w")) == QStringLiteral("1"));
  // sections are case insensitive
  CHECK(Third.SectionExists(QStringLiteral("s")));
}

//---------------------------------------------------------------------------
static void TestCryptography()
{
  // IsValidPassword
  CHECK(Security::IsValidPassword(QString()) == -1);
  CHECK(Security::IsValidPassword(QString(129, QLatin1Char('a'))) == -1);
  CHECK(Security::IsValidPassword(QStringLiteral("abc")) == 0);
  CHECK(Security::IsValidPassword(QStringLiteral("abcdef")) == 0);
  CHECK(Security::IsValidPassword(QStringLiteral("abcde1")) == 1);
  CHECK(Security::IsValidPassword(QStringLiteral("Abcdef")) == 1);

  // scrambling
  for (int Len = 0; Len < 140; Len += 7)
  {
    QString Password = QString(Len, QChar(0x00E4));
    QByteArray Scrambled = Security::ScramblePassword(Password);
    CHECK((Scrambled.size() % 17) == 0);
    QString Back;
    CHECK(Security::UnscramblePassword(Scrambled, Back));
    CHECK(Back == Password);
  }

  // AES with MAC
  QByteArray Encrypted;
  Security::AES256EncryptWithMAC(QByteArray("some data to protect"), QStringLiteral("key"), Encrypted);
  CHECK(Encrypted.size() == 16 + 20 + 10);
  QByteArray Decrypted;
  CHECK(Security::AES256DecryptWithMAC(Encrypted, QStringLiteral("key"), Decrypted));
  CHECK(Decrypted == QByteArray("some data to protect"));
  CHECK(!Security::AES256DecryptWithMAC(Encrypted, QStringLiteral("other"), Decrypted));
  QByteArray Tampered = Encrypted;
  Tampered[20] = static_cast<char>(Tampered[20] ^ 1);
  CHECK(!Security::AES256DecryptWithMAC(Tampered, QStringLiteral("key"), Decrypted));
  QByteArray Verifier;
  Security::AES256CreateVerifier(QStringLiteral("pass"), Verifier);
  CHECK(Verifier.size() == 42);
  CHECK(Security::AES256Verify(QStringLiteral("pass"), Verifier));
  CHECK(!Security::AES256Verify(QStringLiteral("Pass"), Verifier));

  // known answers (interoperability with WinSCP)
  CHECK(Security::AES256Verify(QString::fromLatin1(KnownMasterPassword), Security::HexToBytes(QString::fromLatin1(KnownVerifier))));
  Security::SetMasterPasswordState(true, QString::fromLatin1(KnownVerifier));
  CHECK(Security::UseMasterPassword());
  CHECK(Security::ValidateMasterPassword(QString::fromLatin1(KnownMasterPassword)));
  CHECK(!Security::ValidateMasterPassword(QStringLiteral("wrong")));
  CHECK(Security::IsStronglyEncrypted(QByteArray(KnownExternal)));
  {
    // not known yet, no prompt handler: cannot be decrypted
    Security::NoMasterPasswordPrompt NoPrompt;
    CHECK(Security::DecryptPassword(QByteArray(KnownExternal), QStringLiteral("userhost")).isEmpty());
  }
  // prompt handler supplies the password
  int Prompts = 0;
  Security::SetMasterPasswordPrompt([&Prompts]()
  {
    Prompts++;
    Security::SetMasterPassword(QString::fromLatin1(KnownMasterPassword));
    return true;
  });
  CHECK(Security::DecryptPassword(QByteArray(KnownExternal), QStringLiteral("userhost")) == QString::fromUtf8("s3cret-pässword"));
  CHECK(Prompts == 1);
  CHECK(Security::DecryptPassword(QByteArray(KnownExternalLong), QString()) == QString(60, QLatin1Char('x')));
  CHECK(Prompts == 1);
  CHECK(Security::IsMasterPasswordKnown());
  Security::SetMasterPasswordPrompt(Security::MasterPasswordPromptFunc());
  // new passwords are encrypted with the master password
  QByteArray Stored = Security::EncryptPassword(QStringLiteral("pw"), QStringLiteral("key"));
  CHECK(Security::IsStronglyEncrypted(Stored));
  CHECK(Security::DecryptPassword(Stored, QStringLiteral("key")) == QStringLiteral("pw"));
  CHECK(Security::SamePassword(Stored, Security::EncryptPassword(QStringLiteral("pw"), QStringLiteral("key"))));
  CHECK(!Security::SamePassword(Stored, Security::EncryptPassword(QStringLiteral("pw2"), QStringLiteral("key"))));
  Security::ClearMasterPassword();
  Security::SetMasterPassword(QString());
  CHECK(!Security::UseMasterPassword());
  QByteArray Simple1 = Security::EncryptPassword(QStringLiteral("pw"), QStringLiteral("key"));
  QByteArray Simple2 = Security::EncryptPassword(QStringLiteral("pw"), QStringLiteral("key"));
  CHECK(!Security::IsStronglyEncrypted(Simple1));
  CHECK(Security::SamePassword(Simple1, Simple2));
  CHECK(!Security::SamePassword(Simple1, Security::EncryptPassword(QStringLiteral("pw"), QStringLiteral("key2"))));
}

//---------------------------------------------------------------------------
static QString Child;   // path of this binary

static int RunChild(const QStringList & Args)
{
  QProcess Process;
  Process.setProcessChannelMode(QProcess::ForwardedChannels);
  Process.start(Child, Args);
  if (!Process.waitForFinished(30000))
  {
    Process.kill();
    return -1;
  }
  return Process.exitCode();
}

static QByteArray InitialIni()
{
  QByteArray Password = Security::EncryptPassword(QStringLiteral("alphapw"), QStringLiteral("useralpha.example.com"));
  return
    "[Sessions\\Alpha]\r\n"
    "HostName=alpha.example.com\r\n"
    "UserName=user\r\n"
    "Password=" + Password + "\r\n"
    "Color=8421504\r\n"
    "FutureKey=kept\r\n"
    "\r\n"
    "[Configuration\\Interface]\r\n"
    "QueueTransfersLimit=3\r\n"
    "SomeWindowsOnlyKey=5\r\n"
    "ShowTips=0\r\n"
    "\r\n"
    "[Configuration\\Interface\\Updates]\r\n"
    "Period=7\r\n";
}

static void TestMultiInstance(const QString & Dir)
{
  QString Ini = Dir + QStringLiteral("/multi.ini");
  WriteFile(Ini, InitialIni());
  Configuration & C = Configuration::Instance();
  C.SetSystemIniFileNames(QStringList());
  C.SetIniFileName(Ini);
  C.Load();
  CHECK(C.StoredSessions().FindByName(QStringLiteral("Alpha")) != nullptr);
  CHECK(C.StoredSessions().FindByName(QStringLiteral("Alpha"))->Password == QStringLiteral("alphapw"));
  QString AlphaPassword = LoadIni(Ini).ReadRaw(QStringLiteral("Sessions\\Alpha"), QStringLiteral("Password"));

  int ChangedCount = 0;
  int SessionsChangedCount = 0;
  auto C1 = QObject::connect(&C, &Configuration::Changed, [&ChangedCount]() { ChangedCount++; });
  auto C2 = QObject::connect(&C, &Configuration::StoredSessionsChanged, [&SessionsChangedCount]() { SessionsChangedCount++; });

  // nothing changed: the file is not touched at all
  QByteArray Before = ReadFile(Ini);
  CHECK(C.Save());
  CHECK(ReadFile(Ini) == Before);
  CHECK(ChangedCount == 0);

  // other instance adds a site and changes a preference
  CHECK(RunChild({QStringLiteral("--child-add"), Ini, QStringLiteral("Beta"), QStringLiteral("beta.example.com"), QStringLiteral("7")}) == 0);
  // this instance changes another preference and adds another site
  C.ShowHiddenFiles = true;
  auto Gamma = std::make_shared<SessionData>(C.StoredSessions().DefaultSettings());
  Gamma->Name = QStringLiteral("Gamma");
  Gamma->HostName = QStringLiteral("gamma.example.com");
  Gamma->UserName = QStringLiteral("g");
  Gamma->Password = QStringLiteral("gammapw");
  C.StoredSessions().Add(Gamma);
  CHECK(C.Save());

  IniFile Merged = LoadIni(Ini);
  CHECK(Merged.ReadRaw(QStringLiteral("Sessions\\Beta"), QStringLiteral("HostName")) == QStringLiteral("beta.example.com"));
  CHECK(Merged.ReadRaw(QStringLiteral("Sessions\\Gamma"), QStringLiteral("HostName")) == QStringLiteral("gamma.example.com"));
  CHECK(Merged.ReadRaw(QStringLiteral("Configuration\\Interface"), QStringLiteral("QueueTransfersLimit")) == QStringLiteral("7"));
  CHECK(Merged.ReadRaw(QStringLiteral("Configuration\\Interface"), QStringLiteral("ShowHiddenFiles")) == QStringLiteral("1"));
  // unknown (e.g. Windows-only) keys and unchanged passwords stay untouched
  CHECK(Merged.ReadRaw(QStringLiteral("Configuration\\Interface"), QStringLiteral("SomeWindowsOnlyKey")) == QStringLiteral("5"));
  CHECK(Merged.ReadRaw(QStringLiteral("Configuration\\Interface"), QStringLiteral("ShowTips")) == QStringLiteral("0"));
  CHECK(Merged.ReadRaw(QStringLiteral("Configuration\\Interface\\Updates"), QStringLiteral("Period")) == QStringLiteral("7"));
  CHECK(Merged.ReadRaw(QStringLiteral("Sessions\\Alpha"), QStringLiteral("Color")) == QStringLiteral("8421504"));
  CHECK(Merged.ReadRaw(QStringLiteral("Sessions\\Alpha"), QStringLiteral("FutureKey")) == QStringLiteral("kept"));
  CHECK(Merged.ReadRaw(QStringLiteral("Sessions\\Alpha"), QStringLiteral("Password")) == AlphaPassword);
  // this instance sees the other one's changes now
  CHECK(C.StoredSessions().FindByName(QStringLiteral("Beta")) != nullptr);
  CHECK(C.QueueTransfersLimit == 7);
  CHECK(C.ShowHiddenFiles);
  CHECK(ChangedCount == 1);
  CHECK(SessionsChangedCount == 1);
  CHECK(C.StoredSessions().FindByName(QStringLiteral("Gamma"))->Password == QStringLiteral("gammapw"));

  // deleting here, modifying there
  CHECK(RunChild({QStringLiteral("--child-add"), Ini, QStringLiteral("Beta"), QStringLiteral("beta2.example.com"), QStringLiteral("7")}) == 0);
  C.StoredSessions().Remove(QStringLiteral("Gamma"));
  CHECK(C.Save());
  Merged = LoadIni(Ini);
  CHECK(!Merged.SectionExists(QStringLiteral("Sessions\\Gamma")));
  CHECK(Merged.ReadRaw(QStringLiteral("Sessions\\Beta"), QStringLiteral("HostName")) == QStringLiteral("beta2.example.com"));
  CHECK(C.StoredSessions().FindByName(QStringLiteral("Beta"))->HostName == QStringLiteral("beta2.example.com"));

  // window state is not a preference change
  ChangedCount = 0;
  C.WindowGeometry = QByteArray("geometry");
  CHECK(C.Save());
  CHECK(ChangedCount == 0);
  C.ConfirmDeleting = !C.ConfirmDeleting;
  CHECK(C.Save());
  CHECK(ChangedCount == 1);

  // the file watcher picks up a change made by another instance
  SessionsChangedCount = 0;
  CHECK(RunChild({QStringLiteral("--child-add"), Ini, QStringLiteral("Delta"), QStringLiteral("delta.example.com"), QStringLiteral("8")}) == 0);
  QEventLoop Loop;
  QTimer::singleShot(5000, &Loop, &QEventLoop::quit);
  auto C3 = QObject::connect(&C, &Configuration::StoredSessionsChanged, &Loop, &QEventLoop::quit);
  Loop.exec();
  QObject::disconnect(C3);
  CHECK(SessionsChangedCount == 1);
  CHECK(C.StoredSessions().FindByName(QStringLiteral("Delta")) != nullptr);
  CHECK(C.QueueTransfersLimit == 8);

  QObject::disconnect(C1);
  QObject::disconnect(C2);
}

static int ChildAdd(const QStringList & Args)
{
  // --child-add <ini> <site> <host> <QueueTransfersLimit>
  Configuration & C = Configuration::Instance();
  C.SetSystemIniFileNames(QStringList());
  C.SetIniFileName(Args[0]);
  C.Load();
  auto Data = std::make_shared<SessionData>(C.StoredSessions().DefaultSettings());
  if (auto Existing = C.StoredSessions().FindByName(Args[1]))
  {
    *Data = *Existing;
  }
  Data->Name = Args[1];
  Data->HostName = Args[2];
  C.StoredSessions().Add(Data);
  C.QueueTransfersLimit = Args[3].toInt();
  return C.Save() ? 0 : 1;
}

static int ChildPassword(const QStringList & Args)
{
  // --child-password <ini> <site> <master password or "-"> <expected password>
  Configuration & C = Configuration::Instance();
  C.SetSystemIniFileNames(QStringList());
  C.SetIniFileName(Args[0]);
  QString Master = Args[2];
  if (Master != QStringLiteral("-"))
  {
    Security::SetMasterPasswordPrompt([Master]()
    {
      Security::SetMasterPassword(Master);
      return true;
    });
  }
  C.Load();
  auto Data = C.StoredSessions().FindByName(Args[1]);
  QString Password = Data ? Data->Password : QStringLiteral("<no site>");
  if (Password != Args[3])
  {
    std::printf("child: password '%s', expected '%s'\n", qPrintable(Password), qPrintable(Args[3]));
    return 1;
  }
  // a save by an instance that could not decrypt the password must not lose it
  C.ConfirmRecycling = !C.ConfirmRecycling;
  return C.Save() ? 0 : 1;
}

//---------------------------------------------------------------------------
static void TestMasterPassword(const QString & Dir)
{
  QString Ini = Dir + QStringLiteral("/master.ini");
  WriteFile(Ini, InitialIni());
  Configuration & C = Configuration::Instance();
  C.SetSystemIniFileNames(QStringList());
  C.SetIniFileName(Ini);
  C.Load();
  CHECK(!C.UseMasterPassword());
  QStringList Errors;
  CHECK(C.ChangeMasterPassword(QStringLiteral("Strong#Master1"), &Errors));
  CHECK(Errors.isEmpty());
  CHECK(C.UseMasterPassword());
  IniFile File = LoadIni(Ini);
  CHECK(File.ReadRaw(QStringLiteral("Configuration\\Security"), QStringLiteral("UseMasterPassword")) == QStringLiteral("1"));
  QString Verifier = File.ReadRaw(QStringLiteral("Configuration\\Security"), QStringLiteral("MasterPasswordVerifier"));
  CHECK(Verifier.size() == 84);
  QByteArray StoredAlpha = File.ReadRaw(QStringLiteral("Sessions\\Alpha"), QStringLiteral("Password")).toLatin1();
  CHECK(Security::IsStronglyEncrypted(StoredAlpha));
  CHECK(C.StoredSessions().FindByName(QStringLiteral("Alpha"))->Password == QStringLiteral("alphapw"));

  // another instance knowing the master password decrypts it
  CHECK(RunChild({QStringLiteral("--child-password"), Ini, QStringLiteral("Alpha"), QStringLiteral("Strong#Master1"), QStringLiteral("alphapw")}) == 0);
  // one that does not (prompt cancelled) sees no password, and does not lose it when saving
  CHECK(RunChild({QStringLiteral("--child-password"), Ini, QStringLiteral("Alpha"), QStringLiteral("-"), QString()}) == 0);
  CHECK(LoadIni(Ini).ReadRaw(QStringLiteral("Sessions\\Alpha"), QStringLiteral("Password")).toLatin1() == StoredAlpha);
  // nor does this one (re-reads the changes of the child)
  C.ConfirmDeleting = !C.ConfirmDeleting;
  CHECK(C.Save());
  CHECK(LoadIni(Ini).ReadRaw(QStringLiteral("Sessions\\Alpha"), QStringLiteral("Password")).toLatin1() == StoredAlpha);
  CHECK(C.StoredSessions().FindByName(QStringLiteral("Alpha"))->Password == QStringLiteral("alphapw"));

  // change
  CHECK(C.ChangeMasterPassword(QStringLiteral("Other#Master2"), &Errors));
  CHECK(RunChild({QStringLiteral("--child-password"), Ini, QStringLiteral("Alpha"), QStringLiteral("Other#Master2"), QStringLiteral("alphapw")}) == 0);
  CHECK(RunChild({QStringLiteral("--child-password"), Ini, QStringLiteral("Alpha"), QStringLiteral("Strong#Master1"), QString()}) == 0);

  // clear
  CHECK(C.ClearMasterPassword(&Errors));
  CHECK(!C.UseMasterPassword());
  File = LoadIni(Ini);
  CHECK(File.ReadRaw(QStringLiteral("Configuration\\Security"), QStringLiteral("UseMasterPassword")) == QStringLiteral("0"));
  StoredAlpha = File.ReadRaw(QStringLiteral("Sessions\\Alpha"), QStringLiteral("Password")).toLatin1();
  CHECK(!Security::IsStronglyEncrypted(StoredAlpha));
  CHECK(Security::DecryptPassword(StoredAlpha, QStringLiteral("useralpha.example.com")) == QStringLiteral("alphapw"));
}

//---------------------------------------------------------------------------
static void TestPolicies(const QString & Dir)
{
  QString System = Dir + QStringLiteral("/system.ini");
  WriteFile(System,
    "DisablePasswordStoring=1\r\n"
    "DisableOpenEdit=1\r\n"
    "\r\n"
    "[Configuration\\Interface]\r\n"
    "DarkTheme=1\r\n"
    "QueueTransfersLimit=4\r\n");
  QString Ini = Dir + QStringLiteral("/policies.ini");
  WriteFile(Ini, InitialIni());
  Configuration & C = Configuration::Instance();
  C.SetSystemIniFileNames({System});
  C.SetIniFileName(Ini);
  C.Load();
  CHECK(C.DisablePasswordStoring());
  CHECK(C.DisableOpenEdit());
  CHECK(!C.ForceBanners());
  CHECK(!C.DisableAcceptingHostKeys());
  // machine-wide default below the user's own value
  CHECK(C.DarkTheme == asOff);
  CHECK(C.QueueTransfersLimit == 3);
  CHECK(C.StoredSessions().FindByName(QStringLiteral("Alpha"))->Password.isEmpty());
  QString AlphaPassword = LoadIni(Ini).ReadRaw(QStringLiteral("Sessions\\Alpha"), QStringLiteral("Password"));
  auto Epsilon = std::make_shared<SessionData>(C.StoredSessions().DefaultSettings());
  Epsilon->Name = QStringLiteral("Epsilon");
  Epsilon->HostName = QStringLiteral("epsilon.example.com");
  Epsilon->Password = QStringLiteral("notstored");
  C.StoredSessions().Add(Epsilon);
  CHECK(C.Save());
  IniFile File = LoadIni(Ini);
  CHECK(File.SectionExists(QStringLiteral("Sessions\\Epsilon")));
  CHECK(!File.Keys(QStringLiteral("Sessions\\Epsilon")).contains(QStringLiteral("Password")));
  CHECK(File.ReadRaw(QStringLiteral("Sessions\\Alpha"), QStringLiteral("Password")) == AlphaPassword);
  // machine-wide defaults are not copied to the user's file
  CHECK(!File.Keys(QStringLiteral("Configuration\\Interface")).contains(QStringLiteral("DarkTheme")));
  CHECK(File.Keys(QString()).isEmpty());
  C.SetSystemIniFileNames(QStringList());
}

//---------------------------------------------------------------------------
static void TestExportImport(const QString & Dir)
{
  QString Ini = Dir + QStringLiteral("/export.ini");
  WriteFile(Ini, InitialIni());
  Configuration & C = Configuration::Instance();
  C.SetSystemIniFileNames(QStringList());
  C.SetIniFileName(Ini);
  C.Load();
  C.QueueTransfersLimit = 5;
  QString Backup = Dir + QStringLiteral("/backup.ini");
  QString Error;
  CHECK(C.Export(Backup, &Error));
  IniFile Exported = LoadIni(Backup);
  CHECK(Exported.ReadRaw(QStringLiteral("Configuration\\Interface"), QStringLiteral("QueueTransfersLimit")) == QStringLiteral("5"));
  CHECK(Exported.ReadRaw(QStringLiteral("Sessions\\Alpha"), QStringLiteral("HostName")) == QStringLiteral("alpha.example.com"));
  CHECK(Exported.ReadRaw(QStringLiteral("Sessions\\Alpha"), QStringLiteral("FutureKey")) == QStringLiteral("kept"));
  CHECK(C.DefaultIniFileExportPath().endsWith(QStringLiteral("/export.ini")));

  C.QueueTransfersLimit = 9;
  C.StoredSessions().Remove(QStringLiteral("Alpha"));
  CHECK(C.Save());
  CHECK(!LoadIni(Ini).SectionExists(QStringLiteral("Sessions\\Alpha")));
  int SessionsChanged = 0;
  auto Connection = QObject::connect(&C, &Configuration::StoredSessionsChanged, [&SessionsChanged]() { SessionsChanged++; });
  CHECK(C.Import(Backup, &Error));
  QObject::disconnect(Connection);
  CHECK(SessionsChanged == 1);
  CHECK(C.QueueTransfersLimit == 5);
  CHECK(C.StoredSessions().FindByName(QStringLiteral("Alpha")) != nullptr);
  CHECK(C.StoredSessions().FindByName(QStringLiteral("Alpha"))->Password == QStringLiteral("alphapw"));
  CHECK(LoadIni(Ini).ReadRaw(QStringLiteral("Sessions\\Alpha"), QStringLiteral("HostName")) == QStringLiteral("alpha.example.com"));
  CHECK(!C.Import(Dir + QStringLiteral("/missing.ini"), &Error));
  CHECK(!Error.isEmpty());
}

//---------------------------------------------------------------------------
static void TestMoveStorage(const QString & Dir)
{
  Configuration & C = Configuration::Instance();
  C.SetSystemIniFileNames(QStringList());
  QString Ini = Dir + QStringLiteral("/move.ini");
  WriteFile(Ini, InitialIni());
  C.SetIniFileName(Ini);
  C.Load();
  // overwrite: the current configuration goes to the custom file
  QString Custom = Dir + QStringLiteral("/custom/nixscp-custom.ini");
  CHECK(C.MoveStorage(Custom, false));
  CHECK(C.IniFileName() == Custom);
  CHECK(C.CustomIniFileStorageName() == Custom);
  CHECK(C.ExplicitIniFileStorageName().isEmpty());
  CHECK(LoadIni(Custom).ReadRaw(QStringLiteral("Sessions\\Alpha"), QStringLiteral("HostName")) == QStringLiteral("alpha.example.com"));
  IniFile Override = LoadIni(Configuration::OverrideFileName());
  CHECK(UnMungeStr(Override.ReadRaw(QStringLiteral("Override"), QStringLiteral("IniFile"))) == Custom);
  // use: the configuration is loaded from the other file
  QString Other = Dir + QStringLiteral("/other.ini");
  WriteFile(Other, "[Sessions\\Zeta]\r\nHostName=zeta.example.com\r\n");
  CHECK(C.MoveStorage(Other, true));
  CHECK(C.StoredSessions().FindByName(QStringLiteral("Zeta")) != nullptr);
  CHECK(C.StoredSessions().FindByName(QStringLiteral("Alpha")) == nullptr);
  // back to automatic
  CHECK(C.MoveStorage(QString(), false));
  CHECK(C.IniFileName() == Configuration::AutomaticIniFileName());
  CHECK(!QFile::exists(Configuration::OverrideFileName()));
  CHECK(LoadIni(C.IniFileName()).SectionExists(QStringLiteral("Sessions\\Zeta")));
}

int main(int argc, char * argv[])
{
  // Keep everything (including the automatic INI file and override.ini)
  // away from the user's real ~/.config; the child processes inherit it
  std::unique_ptr<QTemporaryDir> Root;
  if ((argc < 2) || !QString::fromLocal8Bit(argv[1]).startsWith(QStringLiteral("--child")))
  {
    Root.reset(new QTemporaryDir());
    QDir().mkpath(Root->path() + QStringLiteral("/config"));
    QDir().mkpath(Root->path() + QStringLiteral("/sysconfig"));
    qputenv("XDG_CONFIG_HOME", (Root->path() + QStringLiteral("/config")).toLocal8Bit());
    qputenv("XDG_CONFIG_DIRS", (Root->path() + QStringLiteral("/sysconfig")).toLocal8Bit());
  }
  QCoreApplication App(argc, argv);
  QStringList Args = App.arguments().mid(1);
  if (!Args.isEmpty() && (Args.first() == QStringLiteral("--child-add")))
  {
    return ChildAdd(Args.mid(1));
  }
  if (!Args.isEmpty() && (Args.first() == QStringLiteral("--child-password")))
  {
    return ChildPassword(Args.mid(1));
  }
  Child = QCoreApplication::applicationFilePath();

  TestIniStorage();
  TestCryptography();
  QString Dir = Root->path();
  TestMultiInstance(Dir);
  TestMasterPassword(Dir);
  TestPolicies(Dir);
  TestExportImport(Dir);
  TestMoveStorage(Dir);
  if (Failures == 0)
  {
    std::printf("OK (0 failures)\n");
  }
  else
  {
    std::printf("%d failure(s)\n", Failures);
  }
  return (Failures == 0) ? 0 : 1;
}
