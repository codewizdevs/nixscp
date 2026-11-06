// Verification of the settings area (dialogs-settings):
//  - a realistic WinSCP.ini (many sites in folders, workspaces, presets,
//    bookmarks, custom commands, Windows-only keys) is loaded, its values are
//    checked, it is saved again (unknown keys must stay untouched), exported
//    and imported back;
//  - the same configuration as a Windows registry export (.reg, UTF-16, with
//    dword:, hex: and wrapped hex: values) is imported and must give the same
//    preferences, sites and workspaces;
//  - every option of Preferences and of Advanced Site Settings is changed one
//    at a time and must change the WinSCP.ini file (the key is reported);
//  - the dialogs are opened offscreen and screenshotted
//    (NIXSCP_TEST_SCREENSHOT_DIR=<dir>).
// NIXSCP_VERIFY_VERBOSE=1 prints the key written for every option.
#ifndef NIXSCP_GUI_TESTS

#include <cstdio>

int main()
{
  std::printf("SKIP: test_verify_settings needs NIXSCP_GUI_TESTS (Qt Widgets + the GUI sources)\n");
  return 0;
}

#else

#include "core/Bookmarks.h"
#include "core/Configuration.h"
#include "core/CopyParamList.h"
#include "core/CustomCommands.h"
#include "core/IniStorage.h"
#include "core/Security.h"
#include "core/SessionData.h"
#include "core/Workspaces.h"
#include "gui/CopyDialog.h"
#include "gui/CopyParamsDialog.h"
#include "gui/EditMaskDialog.h"
#include "gui/FindFilesDialog.h"
#include "gui/LoginDialog.h"
#include "gui/PreferencesDialog.h"
#include "gui/SiteAdvancedDialog.h"

#include <QAbstractButton>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QToolButton>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QWidget>

#include <cstdio>
#include <functional>

static int Failures = 0;
static QString ScreenshotDir;
static bool Verbose = false;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_EQ(A, B) \
  do { auto VA = (A); auto VB = (B); if (!(VA == VB)) { std::printf("FAIL %s:%d: %s == %s (\"%s\" vs \"%s\")\n", __FILE__, __LINE__, #A, #B, \
    qPrintable(QVariant::fromValue(VA).toString()), qPrintable(QVariant::fromValue(VB).toString())); std::fflush(stdout); Failures++; } } while (0)

static void WriteFile(const QString & Path, const QByteArray & Data)
{
  QFile F(Path);
  if (F.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    F.write(Data);
  }
}

static QByteArray ReadFile(const QString & Path)
{
  QFile F(Path);
  return F.open(QIODevice::ReadOnly) ? F.readAll() : QByteArray();
}

static void Screenshot(QWidget * Widget, const QString & Name)
{
  if (ScreenshotDir.isEmpty())
  {
    return;
  }
  Widget->show();
  QApplication::processEvents();
  Widget->grab().save(QDir(ScreenshotDir).filePath(Name + QStringLiteral(".png")));
}

// Any message box popped up by the code under test is answered with its default
// button, so that a modal loop never blocks the test.
// Handles the next modal dialog instead of the killer (returns true when handled)
static std::function<bool(QWidget *)> ModalHandler;

static void InstallModalKiller()
{
  QTimer * Timer = new QTimer(qApp);
  QObject::connect(Timer, &QTimer::timeout, []()
  {
    QWidget * Modal = QApplication::activeModalWidget();
    if ((Modal != nullptr) && ModalHandler && ModalHandler(Modal))
    {
      return;
    }
    if (QMessageBox * Box = qobject_cast<QMessageBox *>(Modal))
    {
      std::printf("INFO message box: %s\n", qPrintable(Box->text().left(200)));
      Box->done(Box->defaultButton() ? Box->buttonRole(Box->defaultButton()) : 0);
      Box->reject();
    }
    else if (QDialog * Dialog = qobject_cast<QDialog *>(Modal))
    {
      std::printf("INFO modal dialog closed: %s\n", qPrintable(Dialog->windowTitle()));
      Dialog->reject();
    }
  });
  Timer->start(50);
}

// DateTime of WinSCP.ini (BytesToHex of the double, in days)
static QString DateTimeHex(double Seconds)
{
  double Days = Seconds / 86400.0;
  return QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(&Days), sizeof(Days)).toHex().toUpper());
}

//---------------------------------------------------------------------------
// A WinSCP.ini as written by WinSCP 6.x (sections and value formats as WinSCP
// writes them, munged names and values, Windows-only keys NixSCP ignores)
//---------------------------------------------------------------------------
static QByteArray RealisticIni()
{
  QString ProdPassword = QString::fromLatin1(Security::EncryptPassword(QStringLiteral("pr0d-Secret"), QStringLiteral("deployprod.example.com")));
  QString FtpPassword = QString::fromLatin1(Security::EncryptPassword(QStringLiteral("ftp pass"), QStringLiteral("anonymousftp.example.org")));
  QString AdhocPassword = QString::fromLatin1(Security::EncryptPassword(QStringLiteral("adhoc"), QStringLiteral("tempadhoc.example.com")));
  QString Text = QStringLiteral(
    "[Configuration\\Interface]\n"
    "Interface=0\n"
    "ShowHiddenFiles=1\n"
    "FormatSizeBytes=2\n"
    "PanelSearch=1\n"
    "ConfirmDeleting=0\n"
    "ConfirmOverwriting=0\n"
    "CopyOnDoubleClick=1\n"
    "CopyOnDoubleClickConfirmation=1\n"
    "DDTransferConfirmation2=0\n"
    "DDDrives=\n"
    "PathInCaption=1\n"
    "BeepOnFinish=1\n"
    "BeepOnFinishAfter=%BEEP%\n"
    "RefreshRemotePanel=1\n"
    "RefreshRemotePanelInterval=%REFRESH%\n"
    "QueueTransfersLimit=4\n"
    "QueueKeepDoneItems=1\n"
    "QueueKeepDoneItemsFor=60\n"
    "UseLocationProfiles=1\n"
    "DarkTheme=1\n"
    "AutoSaveWorkspace=1\n"
    "AutoWorkspace=My%20workspace\n"
    "AutoSaveWorkspacePasswords=1\n"
    "LastStoredSession=Work/Prod%20server\n"
    "ExternalIpAddress=203.0.113.7\n"
    "LocalPortNumberMin=50000\n"
    "LocalPortNumberMax=50100\n"
    "SessionReopenAuto=10000\n"
    "SessionReopenTimeout=120000\n"
    "PanelFontName=DejaVu%20Sans\n"
    "PanelFontSize=11\n"
    "PanelFontStyle=0\n"
    "PanelFontCharset=1\n"
    "NaturalOrderNumericalSorting=0\n"
    "FullRowSelect=1\n"
    "MinimizeToTray=1\n"
    "LastMonitor=1\n"
    "VersionHistory=6.3.5.14991,6.3.6.15215\n"
    "TipsShown=%TIPS%\n"
    "OpenedStoredSessionFolders=Work,Cloud\n"
    "ContinueOnError=1\n"
    "PuttyPath=%25PROGRAMFILES%25%5CPuTTY%5Cputty.exe\n"
    "PuttyPassword=1\n"
    "TemporaryDirectoryCleanup=0\n"
    "ParallelTransferThreshold=51200\n"
    "[Configuration\\Interface\\Commander]\n"
    "SwappedPanels=1\n"
    "ExplorerStyleSelection=0\n"
    "CompareByTime=1\n"
    "CompareBySize=1\n"
    "ToolbarsLayout=Queue=1::0+-1,Menu=1::0+0\n"
    "LocalPanelWidth=%WIDTH%\n"
    "CurrentPanel=1\n"
    "[Configuration\\Interface\\Commander\\LocalPanel]\n"
    "DirViewParams=0;1;0|150,1;70,1;120,1;150,1;55,0;55,0;@96|5;4;0;1;2;3\n"
    "LastPath=C:%5CUsers%5Cme%5CDocuments\n"
    "[Configuration\\Interface\\Editor]\n"
    "FontName2=Consolas\n"
    "FontSize=11\n"
    "WordWrap=1\n"
    "TabSize=4\n"
    "LargeFileSize=20480\n"
    "[Configuration\\Interface\\Editor\\0]\n"
    "FileMask=*.log\n"
    "Editor=0\n"
    "[Configuration\\Interface\\Editor\\1]\n"
    "FileMask=*.*\n"
    "Editor=1\n"
    "ExternalEditor=%25SystemRoot%25%5Cnotepad.exe\n"
    "ExternalEditorText=1\n"
    "SDIExternalEditor=1\n"
    "DetectMDIExternalEditor=0\n"
    "[Configuration\\Interface\\QueueView]\n"
    "Show=0\n"
    "Height=140\n"
    "[Configuration\\Interface\\CopyParam]\n"
    "TransferMode=2\n"
    "AsciiFileMask=*.*html;%20*.txt;%20*.php\n"
    "ResumeSupport=0\n"
    "ResumeThreshold=204800\n"
    "PreserveTime=1\n"
    "PreserveRights=1\n"
    "Rights=420\n"
    "NewerOnly=1\n"
    "Queue=1\n"
    "CopyParamList=2\n"
    "[Configuration\\Interface\\CopyParam\\0]\n"
    "Name=Upload%20web\n"
    "TransferMode=1\n"
    "IncludeFileMask=*.html;%20*.css%20|%20.git/\n"
    "HasRule=1\n"
    "HostName=*.example.com\n"
    "RemoteDirectory=/var/www/*\n"
    "[Configuration\\Interface\\CopyParam\\1]\n"
    "Name=Binary%20no%20time\n"
    "TransferMode=0\n"
    "PreserveTime=0\n"
    "ResumeThreshold=1048576\n"
    "[Configuration\\Interface\\NewDirectory2]\n"
    "Rights=493\n"
    "[Configuration\\Interface\\Updates]\n"
    "Period=7\n"
    "LastCheck=%TIPS%\n"
    "[Configuration\\Logging]\n"
    "Logging=1\n"
    "LogFileName=%25TEMP%25%5C!S.log\n"
    "LogFileAppend=0\n"
    "LogProtocol=1\n"
    "LogMaxSize=1048576\n"
    "LogMaxCount=3\n"
    "[Configuration\\Security]\n"
    "UseMasterPassword=0\n"
    "MasterPasswordVerifier=\n"
    "[Configuration\\Bookmarks\\Local\\Work/Prod%20server]\n"
    "Logs=C:%5Clogs\n"
    "[Configuration\\Bookmarks\\Remote\\Work/Prod%20server]\n"
    "Logs=/var/log\n"
    "[Configuration\\Bookmarks\\Remote\\Work/Prod%20server\\Deploy]\n"
    "Releases=/srv/releases\n"
    "[Configuration\\CustomCommands]\n"
    "&Touch=touch%20\"!\"\n"
    "Tail log=tail%20-n%20100%20\"!\"\n"
    "[Configuration\\CustomCommandsParams]\n"
    "&Touch=3\n"
    "Tail log=8\n"
    "[Configuration\\CDCache]\n"
    "deploy@prod.example.com=%2Fvar%2Fwww\n"
    "[Sessions\\Default%20Settings]\n"
    "UserName=defuser\n"
    "Timeout=30\n"
    "[Sessions\\Work/Prod%20server]\n"
    "HostName=prod.example.com\n"
    "PortNumber=2200\n"
    "UserName=deploy\n"
    "FSProtocol=2\n"
    "Password=%PWD1%\n"
    "PublicKeyFile=C:%5CUsers%5Cme%5C.ssh%5Cid_ed25519.ppk\n"
    "LocalDirectory=C:%5CProjects%5Cweb\n"
    "RemoteDirectory=/var/www/html\n"
    "SynchronizeBrowsing=1\n"
    "UpdateDirectories=1\n"
    "Color=255\n"
    "Note=Production%20server%0D%0ABe%20careful\n"
    "Compression=1\n"
    "PingType=2\n"
    "PingIntervalSecs=15\n"
    "Timeout=45\n"
    "Cipher=aes,chacha20,aesgcm,3des,WARN,des,blowfish,arcfour\n"
    "KEX=ntru-curve25519,ecdh,mlkem-curve25519,dh-gex-sha1,dh-group18-sha512,dh-group17-sha512,dh-group16-sha512,dh-group15-sha512,dh-group14-sha1,rsa,WARN,dh-group1-sha1\n"
    "SshHostKey=ssh-ed25519%20255%20SHA256:abcdefghijklmnopqrstuvwxyz0123456789ABCDEFG\n"
    "Shell=/bin/bash\n"
    "EOLType=0\n"
    "TimeDifference=0\n"
    "[Sessions\\Work/Staging]\n"
    "HostName=staging.example.com\n"
    "UserName=deploy\n"
    "FSProtocol=0\n"
    "LookupUserGroups2=2\n"
    "SCPLsFullTime=1\n"
    "ClearAliases=0\n"
    "UnsetNationalVars=0\n"
    "ListingCommand=ls%20-la%20--full-time\n"
    "Tunnel=1\n"
    "TunnelHostName=jump.example.com\n"
    "TunnelUserName=jumper\n"
    "TunnelPortNumber=2222\n"
    "TunnelLocalPortNumber=0\n"
    "[Sessions\\Work/Old/FTP%20archive]\n"
    "HostName=ftp.example.org\n"
    "UserName=anonymous\n"
    "FSProtocol=5\n"
    "Ftps=3\n"
    "PortNumber=21\n"
    "Password=%PWD2%\n"
    "FtpPasvMode=0\n"
    "FtpListAll=0\n"
    "FtpUseMlsd=1\n"
    "FtpForcePasvIp2=1\n"
    "Utf=1\n"
    "PostLoginCommands=SITE%20UMASK%20022\n"
    "RemoteDirectory=/pub\n"
    "ProxyMethod=3\n"
    "ProxyHost=proxy.example.org\n"
    "ProxyPort=3128\n"
    "ProxyUsername=me\n"
    "[Sessions\\Cloud/Nextcloud]\n"
    "HostName=cloud.example.net\n"
    "FSProtocol=6\n"
    "Ftps=1\n"
    "PortNumber=443\n"
    "UserName=me\n"
    "RemoteDirectory=/remote.php/dav/files/me\n"
    "[Sessions\\Cloud/S3%20bucket]\n"
    "HostName=s3.amazonaws.com\n"
    "FSProtocol=7\n"
    "Ftps=1\n"
    "PortNumber=443\n"
    "UserName=AKIAIOSFODNN7EXAMPLE\n"
    "S3DefaultRegion=eu-central-1\n"
    "S3UrlStyle=1\n"
    "RemoteDirectory=/my-bucket\n"
    "[Sessions\\%EF%BB%BF%C3%9Cn%C3%AFcode%20site]\n"
    "HostName=xn--nicode-cua.example\n"
    "UserName=%EF%BB%BF%C3%BCser\n"
    "[Sessions\\My%20workspace/0000]\n"
    "IsWorkspace=1\n"
    "Link=Work/Prod%20server\n"
    "LocalDirectory=C:%5CProjects%5Cweb%5Capp\n"
    "RemoteDirectory=/var/www/html/app\n"
    "[Sessions\\My%20workspace/0001]\n"
    "HostName=adhoc.example.com\n"
    "UserName=temp\n"
    "FSProtocol=2\n"
    "IsWorkspace=1\n"
    "Password=%PWD3%\n"
    "RemoteDirectory=/tmp\n"
    "[Sessions\\My%20workspace/0002]\n"
    "IsWorkspace=1\n"
    "Link=Cloud/S3%20bucket\n"
    "RemoteDirectory=/my-bucket/logs\n"
    "[Sessions\\Second%20WS/0000]\n"
    "IsWorkspace=1\n"
    "Link=Work/Staging\n"
    "[SshHostKeys]\n"
    "ssh-ed25519@2200:prod.example.com=0x1f2e3d4c5b6a79881f2e3d4c5b6a79881f2e3d4c5b6a79881f2e3d4c5b6a7988\n"
    "[FtpsCertificates]\n"
    "ftp.example.org:21=00:11:22:33:44:55:66:77:88:99:aa:bb:cc:dd:ee:ff:00:11:22:33\n");
  Text.replace(QStringLiteral("%PWD1%"), ProdPassword);
  Text.replace(QStringLiteral("%PWD2%"), FtpPassword);
  Text.replace(QStringLiteral("%PWD3%"), AdhocPassword);
  Text.replace(QStringLiteral("%BEEP%"), DateTimeHex(45));
  Text.replace(QStringLiteral("%REFRESH%"), DateTimeHex(120));
  Text.replace(QStringLiteral("%TIPS%"), DateTimeHex(45678.5 * 86400));
  Text.replace(QStringLiteral("%WIDTH%"), QString::fromLatin1(QByteArray::fromRawData("\x00\x00\x00\x00\x00\x00\xe0\x3f", 8).toHex().toUpper()));
  return Text.toUtf8();
}

//---------------------------------------------------------------------------
// The same settings as "reg export HKCU\Software\Martin Prikryl\WinSCP 2" writes
// them: TRegistryStorage keeps integers/booleans as REG_DWORD, Int64 and
// DateTime/Float as REG_BINARY (hex:, wrapped at 80 columns), strings as REG_SZ
//---------------------------------------------------------------------------
static QString RegHexList(const QByteArray & Bytes, const QString & Prefix)
{
  QString Result;
  int Column = Prefix.size();
  for (int Index = 0; Index < Bytes.size(); Index++)
  {
    QString Byte = QString::fromLatin1(QByteArray(1, Bytes[Index]).toHex());
    if (Index > 0)
    {
      Result += QLatin1Char(',');
      Column++;
      // reg export wraps at 80 columns; narrower here, so that the 8 byte values are wrapped too
      if (Column > 44)
      {
        Result += QStringLiteral("\\\r\n  ");
        Column = 2;
      }
    }
    Result += Byte;
    Column += 2;
  }
  return Result;
}

static QByteArray IniToReg(const IniFile & Ini)
{
  static const QStringList Int64Names = { QStringLiteral("LogMaxSize"), QStringLiteral("ResumeThreshold") };
  static const QStringList BinaryNames = { QStringLiteral("BeepOnFinishAfter"), QStringLiteral("RefreshRemotePanelInterval"),
    QStringLiteral("TipsShown"), QStringLiteral("LocalPanelWidth"), QStringLiteral("LastCheck") };
  // strings which look like numbers still are REG_SZ in the registry
  static const QStringList StringNames = { QStringLiteral("Name"), QStringLiteral("Password"), QStringLiteral("HostName") };
  static const QString Root = QStringLiteral("HKEY_CURRENT_USER\\Software\\Martin Prikryl\\WinSCP 2");
  QString Text = QStringLiteral("Windows Registry Editor Version 5.00\r\n\r\n[%1]\r\n\r\n").arg(Root);
  for (const QString & Section : Ini.Sections())
  {
    Text += QStringLiteral("[%1\\%2]\r\n").arg(Root, Section);
    for (const QString & Key : Ini.Keys(Section))
    {
      QString Value = Ini.ReadRaw(Section, Key);
      QString Name = Key;
      Name.replace(QStringLiteral("\\"), QStringLiteral("\\\\")).replace(QStringLiteral("\""), QStringLiteral("\\\""));
      QString Prefix = QStringLiteral("\"%1\"=hex:").arg(Name);
      bool IsNumber = false;
      int Number = Value.toInt(&IsNumber);
      if (Int64Names.contains(Key))
      {
        qint64 V = Value.toLongLong();
        Text += Prefix + RegHexList(QByteArray(reinterpret_cast<const char *>(&V), sizeof(V)), Prefix) + QStringLiteral("\r\n");
      }
      else if (BinaryNames.contains(Key))
      {
        Text += Prefix + RegHexList(QByteArray::fromHex(Value.toLatin1()), Prefix) + QStringLiteral("\r\n");
      }
      else if (IsNumber && !StringNames.contains(Key) && !Value.startsWith(QLatin1Char('+')))
      {
        Text += QStringLiteral("\"%1\"=dword:%2\r\n").arg(Name, QStringLiteral("%1").arg(static_cast<quint32>(Number), 8, 16, QLatin1Char('0')));
      }
      else
      {
        QString V = Value;
        V.replace(QStringLiteral("\\"), QStringLiteral("\\\\")).replace(QStringLiteral("\""), QStringLiteral("\\\""));
        Text += QStringLiteral("\"%1\"=\"%2\"\r\n").arg(Name, V);
      }
    }
    Text += QStringLiteral("\r\n");
  }
  QByteArray Result("\xFF\xFE", 2);
  Result.append(reinterpret_cast<const char *>(Text.utf16()), Text.size() * 2);
  return Result;
}

//---------------------------------------------------------------------------
// Canonical form of a site / of all sites (as NixSCP saves them)
static QString SessionDump(const SessionData & Data)
{
  // encrypted passwords differ by their random salt: compare them decrypted
  static const QStringList PasswordKeys = { QStringLiteral("Password"), QStringLiteral("ProxyPasswordEnc"),
    QStringLiteral("TunnelPassword"), QStringLiteral("TunnelPassphrase"), QStringLiteral("Passphrase"), QStringLiteral("EncryptKey") };
  IniFile File;
  IniStorage Storage(&File);
  Storage.OpenSubKey(QStringLiteral("S"), true);
  SessionData Defaults;
  Data.Save(Storage, &Defaults);
  QStringList Lines;
  for (const QString & Key : File.Keys(QStringLiteral("S")))
  {
    if (!PasswordKeys.contains(Key))
    {
      Lines.append(Key + QLatin1Char('=') + File.ReadRaw(QStringLiteral("S"), Key));
    }
  }
  Lines.append(QStringLiteral("(passwords)=") + Data.Password + QLatin1Char('/') + Data.ProxyPassword + QLatin1Char('/') + Data.TunnelPassword);
  Lines.sort();
  return Data.Name + QLatin1Char('|') + Lines.join(QLatin1Char('|'));
}

static QStringList SessionsDump(StoredSessionList & List)
{
  QStringList Result;
  for (const auto & Data : List.Sessions())
  {
    Result.append(SessionDump(*Data));
  }
  Result.append(SessionDump(List.DefaultSettings()));
  Result.sort();
  return Result;
}

// All (section, key, value) triples of a file
static QSet<QString> IniTriples(const IniFile & Ini)
{
  QSet<QString> Result;
  for (const QString & Section : Ini.Sections())
  {
    for (const QString & Key : Ini.Keys(Section))
    {
      Result.insert(Section + QStringLiteral(" :: ") + Key + QLatin1Char('=') + Ini.ReadRaw(Section, Key));
    }
  }
  return Result;
}

static QStringList IniDiff(const QByteArray & Before, const QByteArray & After)
{
  IniFile B, A;
  B.LoadFromData(Before);
  A.LoadFromData(After);
  QSet<QString> TB = IniTriples(B), TA = IniTriples(A);
  QStringList Result;
  for (const QString & T : TA)
  {
    if (!TB.contains(T))
    {
      Result.append(QStringLiteral("+") + T);
    }
  }
  for (const QString & T : TB)
  {
    if (!TA.contains(T))
    {
      Result.append(QStringLiteral("-") + T);
    }
  }
  Result.sort();
  return Result;
}

#define CHECK_LIST(A, B) \
  do { QStringList LA = (A), LB = (B); if (LA != LB) { std::printf("FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #A, #B); \
    for (const QString & X : LA) if (!LB.contains(X)) std::printf("   only left:  %s\n", qPrintable(X)); \
    for (const QString & X : LB) if (!LA.contains(X)) std::printf("   only right: %s\n", qPrintable(X)); \
    std::fflush(stdout); Failures++; } } while (0)

//---------------------------------------------------------------------------
static void CheckRealisticConfiguration(Configuration & C, const QString & Context)
{
  std::printf("-- checking loaded configuration (%s)\n", qPrintable(Context));
  CHECK(C.ShowHiddenFiles);
  CHECK_EQ(static_cast<int>(C.FormatSizeBytes), static_cast<int>(fbShort));
  CHECK_EQ(static_cast<int>(C.PanelSearch), static_cast<int>(isName));
  CHECK(!C.ConfirmDeleting);
  CHECK(!C.DefaultCopyParam().ConfirmOverwriting);
  CHECK_EQ(static_cast<int>(C.DoubleClickAction), static_cast<int>(dcaCopy));
  CHECK(C.CopyOnDoubleClick);
  CHECK(C.CopyOnDoubleClickConfirmation);
  // TAutoSwitch: asOn = 0, asOff = 1, asAuto = 2
  CHECK_EQ(static_cast<int>(C.DDTransferConfirmation), static_cast<int>(asOn));
  CHECK_EQ(static_cast<int>(C.PathInCaption), static_cast<int>(picFull));
  CHECK(C.BeepOnFinish);
  CHECK_EQ(C.BeepOnFinishAfter, 45);
  CHECK(C.RefreshRemotePanel);
  CHECK_EQ(C.RefreshRemotePanelInterval, 120);
  CHECK_EQ(C.QueueTransfersLimit, 4);
  CHECK_EQ(C.QueueKeepDoneItemsFor, 60);
  CHECK(C.UseLocationProfiles);
  CHECK_EQ(static_cast<int>(C.DarkTheme), static_cast<int>(asOff));
  CHECK(C.AutoSaveWorkspace);
  CHECK_EQ(C.AutoWorkspace, QStringLiteral("My workspace"));
  CHECK_EQ(C.ExternalIpAddress, QStringLiteral("203.0.113.7"));
  CHECK_EQ(C.LocalPortNumberMin, 50000);
  CHECK_EQ(C.LocalPortNumberMax, 50100);
  CHECK_EQ(C.SessionReopenAuto, 10000);
  CHECK_EQ(C.SessionReopenTimeout, 120000);
  CHECK_EQ(C.PanelFontName, QStringLiteral("DejaVu Sans"));
  CHECK_EQ(C.PanelFontSize, 11);
  CHECK(!C.NaturalOrderNumericalSorting);
  CHECK(C.FullRowSelect);
  CHECK(C.MinimizeToTray);
  CHECK(C.ContinueOnError);
  CHECK_EQ(C.PuttyPath, QStringLiteral("%PROGRAMFILES%\\PuTTY\\putty.exe"));
  CHECK(C.PuttyPassword);
  CHECK(!C.TemporaryDirectoryCleanup);
  CHECK_EQ(C.ParallelTransferThreshold, 51200);
  CHECK(C.SwappedPanels);
  CHECK_EQ(static_cast<int>(C.NortonLikeMode), static_cast<int>(nlOn));
  CHECK(C.CompareBySize);
  CHECK_EQ(C.EditorFontName, QStringLiteral("Consolas"));
  CHECK_EQ(C.EditorFontSize, 11);
  CHECK(C.EditorWordWrap);
  CHECK_EQ(C.EditorTabSize, 4);
  CHECK_EQ(C.EditorLargeFileSize, 20480);
  CHECK_EQ(C.Editors.size(), 2);
  if (C.Editors.size() == 2)
  {
    CHECK_EQ(C.Editors[0].FileMask, QStringLiteral("*.log"));
    CHECK_EQ(static_cast<int>(C.Editors[1].Editor), static_cast<int>(edExternal));
    CHECK_EQ(C.Editors[1].ExternalEditor, QStringLiteral("%SystemRoot%\\notepad.exe"));
    CHECK(C.Editors[1].ExternalEditorText);
    CHECK(C.Editors[1].SDIExternalEditor);
  }
  CHECK_EQ(static_cast<int>(C.QueueViewShow), static_cast<int>(qvShow));
  CopyParam & P = C.DefaultCopyParam();
  CHECK_EQ(static_cast<int>(P.Mode), static_cast<int>(CopyParam::tmAutomatic));
  CHECK_EQ(static_cast<int>(P.ResumeSupport), static_cast<int>(CopyParam::rsOn));
  CHECK_EQ(P.ResumeThreshold, static_cast<qint64>(204800));
  CHECK(P.PreserveRights);
  CHECK_EQ(P.Rights, 0644u);
  CHECK(P.NewerOnly);
  CHECK(P.Queue);
  CHECK(C.Logging);
  CHECK_EQ(C.LogFileName, QStringLiteral("%TEMP%\\!S.log"));
  CHECK(!C.LogFileAppend);
  CHECK_EQ(C.LogProtocol, 1);
  CHECK_EQ(C.LogMaxSize, static_cast<qint64>(1048576));
  CHECK_EQ(C.LogMaxCount, 3);

  // transfer presets
  CopyParamPresets::Instance().Reload();
  const CopyParamList & Presets = CopyParamPresets::Instance().List();
  CHECK_EQ(Presets.Count(), 2);
  if (Presets.Count() == 2)
  {
    CHECK_EQ(Presets.Name(0), QStringLiteral("Upload web"));
    CHECK_EQ(static_cast<int>(Presets.Param(0).Mode), static_cast<int>(CopyParam::tmAscii));
    CHECK(Presets.Rule(0) != nullptr);
    CopyParamRuleData RuleData;
    RuleData.HostName = QStringLiteral("www.example.com");
    RuleData.RemoteDirectory = QStringLiteral("/var/www/site");
    CHECK_EQ(Presets.Find(RuleData), 0);
    CHECK_EQ(Presets.Name(1), QStringLiteral("Binary no time"));
    CHECK(!Presets.Param(1).PreserveTime);
    CHECK_EQ(Presets.Param(1).ResumeThreshold, static_cast<qint64>(1048576));
  }

  // custom commands
  CustomCommandList Commands = CustomCommandList::Configured();
  const CustomCommandType * Touch = Commands.Find(QStringLiteral("&Touch"));
  CHECK(Touch != nullptr);
  if (Touch != nullptr)
  {
    CHECK_EQ(Touch->Command, QStringLiteral("touch \"!\""));
    CHECK_EQ(Touch->Params, 3);
  }
  CHECK(Commands.Find(QStringLiteral("Tail log")) != nullptr);

  // bookmarks
  BookmarkList Marks = Bookmarks::ConfiguredList(QStringLiteral("Work/Prod server"));
  const Bookmark * Logs = Marks.FindByName(QString(), QStringLiteral("Logs"));
  CHECK(Logs != nullptr);
  if (Logs != nullptr)
  {
    CHECK_EQ(Logs->Remote, QStringLiteral("/var/log"));
    CHECK_EQ(Logs->Local, QStringLiteral("C:\\logs"));
  }
  const Bookmark * Releases = Marks.FindByName(QStringLiteral("Deploy"), QStringLiteral("Releases"));
  CHECK((Releases != nullptr) && (Releases->Remote == QStringLiteral("/srv/releases")));

  // sites
  StoredSessionList & S = C.StoredSessions();
  CHECK_EQ(S.DefaultSettings().UserName, QStringLiteral("defuser"));
  CHECK_EQ(S.DefaultSettings().Timeout, 30);
  auto Prod = S.FindByName(QStringLiteral("Work/Prod server"));
  CHECK(Prod != nullptr);
  if (Prod)
  {
    CHECK_EQ(Prod->HostName, QStringLiteral("prod.example.com"));
    CHECK_EQ(Prod->PortNumber, 2200);
    CHECK_EQ(Prod->Password, QStringLiteral("pr0d-Secret"));
    CHECK_EQ(static_cast<int>(Prod->FSProtocol), static_cast<int>(fsSFTPonly));
    CHECK_EQ(Prod->PublicKeyFile, QStringLiteral("C:\\Users\\me\\.ssh\\id_ed25519.ppk"));
    CHECK_EQ(Prod->LocalDirectory, QStringLiteral("C:\\Projects\\web"));
    CHECK_EQ(Prod->RemoteDirectory, QStringLiteral("/var/www/html"));
    CHECK(Prod->SynchronizeBrowsing);
    CHECK_EQ(Prod->Color, 255);
    CHECK_EQ(Prod->Note, QStringLiteral("Production server\r\nBe careful"));
    CHECK(Prod->Compression);
    CHECK_EQ(Prod->Timeout, 45);
    CHECK_EQ(Prod->FolderName(), QStringLiteral("Work"));
  }
  auto Staging = S.FindByName(QStringLiteral("Work/Staging"));
  CHECK(Staging && Staging->Tunnel && (Staging->TunnelHostName == QStringLiteral("jump.example.com")) &&
        (Staging->TunnelPortNumber == 2222) && (Staging->FSProtocol == fsSCPonly));
  CHECK(Staging && (Staging->UserName == QStringLiteral("deploy")));
  auto Ftp = S.FindByName(QStringLiteral("Work/Old/FTP archive"));
  CHECK(Ftp && (Ftp->FSProtocol == fsFTP) && (Ftp->Ftps == ftpsExplicitTls) && (Ftp->Password == QStringLiteral("ftp pass")));
  CHECK(Ftp && (Ftp->ProxyMethod == pmHTTP) && (Ftp->ProxyHost == QStringLiteral("proxy.example.org")) && (Ftp->ProxyPort == 3128));
  CHECK(Ftp && (Ftp->PostLoginCommands == QStringLiteral("SITE UMASK 022")));
  auto Dav = S.FindByName(QStringLiteral("Cloud/Nextcloud"));
  CHECK(Dav && (Dav->FSProtocol == fsWebDAV) && (Dav->Ftps == ftpsImplicit) && (Dav->PortNumber == 443));
  auto S3 = S.FindByName(QStringLiteral("Cloud/S3 bucket"));
  CHECK(S3 && (S3->FSProtocol == fsS3) && (S3->S3DefaultRegion == QStringLiteral("eu-central-1")));
  auto Unicode = S.FindByName(QString::fromUtf8("\xC3\x9Cn\xC3\xAF" "code site"));
  CHECK(Unicode && (Unicode->UserName == QString::fromUtf8("\xC3\xBCser")));
  // the default settings apply to sites that do not override them
  auto Nas = S.FindByName(QStringLiteral("Cloud/Nextcloud"));
  CHECK(Nas && (Nas->Timeout == 30));

  // folders and workspaces
  CHECK(S.IsFolder(QStringLiteral("Work")) || Workspaces::IsFolder(S, QStringLiteral("Work")));
  CHECK(Workspaces::IsFolder(S, QStringLiteral("Work/Old")));
  QStringList WorkspaceNames = Workspaces::WorkspaceNames(S);
  CHECK_EQ(WorkspaceNames, (QStringList{QStringLiteral("My workspace"), QStringLiteral("Second WS")}));
  QList<SessionData> Tabs = Workspaces::LoadWorkspace(S, QStringLiteral("My workspace"));
  CHECK_EQ(Tabs.size(), 3);
  if (Tabs.size() == 3)
  {
    CHECK_EQ(Tabs[0].HostName, QStringLiteral("prod.example.com"));
    CHECK_EQ(Tabs[0].RemoteDirectory, QStringLiteral("/var/www/html/app"));
    CHECK_EQ(Tabs[0].LocalDirectory, QStringLiteral("C:\\Projects\\web\\app"));
    CHECK_EQ(Tabs[0].Password, QStringLiteral("pr0d-Secret"));
    CHECK_EQ(Tabs[1].HostName, QStringLiteral("adhoc.example.com"));
    CHECK_EQ(Tabs[1].Password, QStringLiteral("adhoc"));
    CHECK_EQ(Tabs[2].HostName, QStringLiteral("s3.amazonaws.com"));
    CHECK_EQ(Tabs[2].RemoteDirectory, QStringLiteral("/my-bucket/logs"));
  }
  CHECK_EQ(Workspaces::LoadWorkspace(S, QStringLiteral("Second WS")).size(), 1);
}

static void TestIniRoundTrip(const QString & Dir)
{
  std::printf("== WinSCP.ini round trip\n");
  QString IniPath = QDir(Dir).filePath(QStringLiteral("WinSCP.ini"));
  QByteArray Original = RealisticIni();
  WriteFile(IniPath, Original);
  Configuration & C = Configuration::Instance();
  C.SetIniFileName(IniPath);
  C.Load();
  CheckRealisticConfiguration(C, QStringLiteral("WinSCP.ini"));
  QStringList SitesBefore = SessionsDump(C.StoredSessions());

  // saving without changes must not change anything
  CHECK(C.Save());
  QStringList Diff = IniDiff(Original, ReadFile(IniPath));
  for (const QString & Line : Diff)
  {
    std::printf("  save without changes: %s\n", qPrintable(Line));
  }
  // only added keys (values NixSCP writes that were not there) are acceptable
  for (const QString & Line : Diff)
  {
    CHECK(Line.startsWith(QLatin1Char('+')));
  }

  // one change: only that key changes, everything else (Windows-only keys,
  // passwords, binary values, workspaces) stays
  C.ShowHiddenFiles = false;
  CHECK(C.Save());
  QByteArray Saved = ReadFile(IniPath);
  Diff = IniDiff(Original, Saved);
  for (const QString & Line : Diff)
  {
    if (Line.startsWith(QLatin1Char('-')))
    {
      CHECK_EQ(Line, QStringLiteral("-Configuration\\Interface :: ShowHiddenFiles=1"));
    }
  }
  CHECK(Diff.contains(QStringLiteral("+Configuration\\Interface :: ShowHiddenFiles=0")));

  // reload from disk: identical sites
  C.Load();
  CHECK_LIST(SessionsDump(C.StoredSessions()), SitesBefore);
  C.ShowHiddenFiles = true;
  CHECK(C.Save());

  // Export, then Import into an empty configuration
  QString ExportPath = QDir(Dir).filePath(QStringLiteral("export.ini"));
  QString Error;
  CHECK(C.Export(ExportPath, &Error));
  CHECK(Error.isEmpty());
  QString IniPath2 = QDir(Dir).filePath(QStringLiteral("empty.ini"));
  WriteFile(IniPath2, QByteArray());
  C.SetIniFileName(IniPath2);
  C.Load();
  CHECK(C.StoredSessions().Sessions().isEmpty());
  CHECK(C.Import(ExportPath, &Error));
  CHECK(Error.isEmpty());
  CheckRealisticConfiguration(C, QStringLiteral("exported and imported"));
  CHECK_LIST(SessionsDump(C.StoredSessions()), SitesBefore);
  // imported content is saved to the new file
  C.Load();
  CHECK_LIST(SessionsDump(C.StoredSessions()), SitesBefore);
  {
    IniFile Imported;
    Imported.Load(IniPath2);
    CHECK_EQ(Imported.ReadRaw(QStringLiteral("Configuration\\Interface\\Commander\\LocalPanel"), QStringLiteral("LastPath")),
      QStringLiteral("C:%5CUsers%5Cme%5CDocuments"));
    CHECK_EQ(Imported.ReadRaw(QStringLiteral("SshHostKeys"), QStringLiteral("ssh-ed25519@2200:prod.example.com")).left(10),
      QStringLiteral("0x1f2e3d4c"));
  }
  C.SetIniFileName(IniPath);
  C.Load();
}

static void TestRegImport(const QString & Dir)
{
  std::printf("== .reg import\n");
  IniFile Ini;
  Ini.LoadFromData(RealisticIni());
  QString RegPath = QDir(Dir).filePath(QStringLiteral("winscp.reg"));
  WriteFile(RegPath, IniToReg(Ini));

  // reference: the WinSCP.ini
  QString IniPath = QDir(Dir).filePath(QStringLiteral("reference.ini"));
  WriteFile(IniPath, RealisticIni());
  Configuration & C = Configuration::Instance();
  C.SetIniFileName(IniPath);
  C.Load();
  ConfigurationData Reference = C;
  CopyParam ReferenceCopyParam = C.DefaultCopyParam();
  QStringList ReferenceSites = SessionsDump(C.StoredSessions());

  // the .reg parsed by IniFile gives the same values as the INI
  IniFile Reg;
  CHECK(Reg.Load(RegPath));
  QSet<QString> IniT = IniTriples(Ini), RegT = IniTriples(Reg);
  for (const QString & T : IniT)
  {
    if (!RegT.contains(T))
    {
      std::printf("  missing in .reg import: %s\n", qPrintable(T));
      Failures++;
    }
  }

  // Tools > Import/Restore Configuration from the .reg
  QString Target = QDir(Dir).filePath(QStringLiteral("fromreg.ini"));
  WriteFile(Target, QByteArray());
  C.SetIniFileName(Target);
  C.Load();
  QString Error;
  CHECK(C.Import(RegPath, &Error));
  CHECK(Error.isEmpty());
  CheckRealisticConfiguration(C, QStringLiteral(".reg"));
  CHECK(static_cast<ConfigurationData &>(C) == Reference);
  CHECK(C.DefaultCopyParam() == ReferenceCopyParam);
  CHECK_LIST(SessionsDump(C.StoredSessions()), ReferenceSites);

  // Login dialog > Tools > Import Sites from the .reg: sites only (also workspaces)
  StoredSessionList List;
  int Count = List.ImportFromWinSCPIni(RegPath, &Error);
  std::printf("  ImportFromWinSCPIni: %d sites\n", Count);
  CHECK(Count >= 6);
  CHECK(List.FindByName(QStringLiteral("Work/Old/FTP archive")) != nullptr);
  auto Prod = List.FindByName(QStringLiteral("Work/Prod server"));
  CHECK(Prod && (Prod->Password == QStringLiteral("pr0d-Secret")));
  C.SetIniFileName(IniPath);
  C.Load();
}

//---------------------------------------------------------------------------
// Option coverage: every control changed alone must change the file
//---------------------------------------------------------------------------
struct ControlRef
{
  QString Description;
  int Index;
};

static QString ControlCaption(QWidget * W)
{
  QString Caption;
  if (QAbstractButton * B = qobject_cast<QAbstractButton *>(W))
  {
    Caption = B->text();
  }
  else if (QGroupBox * G = qobject_cast<QGroupBox *>(W))
  {
    Caption = G->title();
  }
  if (Caption.isEmpty())
  {
    // buddy label / group box
    for (QWidget * P = W->parentWidget(); P != nullptr; P = P->parentWidget())
    {
      if (QGroupBox * G = qobject_cast<QGroupBox *>(P))
      {
        Caption = QStringLiteral("[") + G->title() + QStringLiteral("] ");
        break;
      }
    }
    Caption += QString::fromLatin1(W->metaObject()->className()) + QLatin1Char('#') + W->objectName();
    if (QComboBox * Combo = qobject_cast<QComboBox *>(W))
    {
      Caption += QStringLiteral(" (") + Combo->currentText() + QLatin1Char(')');
    }
  }
  Caption.remove(QLatin1Char('&'));
  return Caption;
}

static QList<QWidget *> OptionControls(QWidget * Dialog)
{
  QList<QWidget *> Result;
  for (QWidget * W : Dialog->findChildren<QWidget *>())
  {
    bool IsOption =
      qobject_cast<QCheckBox *>(W) || qobject_cast<QRadioButton *>(W) ||
      (qobject_cast<QComboBox *>(W) && !qobject_cast<QComboBox *>(W)->isEditable()) ||
      qobject_cast<QSpinBox *>(W) || (qobject_cast<QGroupBox *>(W) && qobject_cast<QGroupBox *>(W)->isCheckable());
    if (IsOption)
    {
      Result.append(W);
    }
  }
  return Result;
}

// Changes the control; false when it cannot be changed (disabled, single item...)
static bool ChangeControl(QWidget * W)
{
  if (!W->isEnabled())
  {
    return false;
  }
  if (QCheckBox * Check = qobject_cast<QCheckBox *>(W))
  {
    Check->setCheckState((Check->checkState() == Qt::Checked) ? Qt::Unchecked : Qt::Checked);
    return true;
  }
  if (QRadioButton * Radio = qobject_cast<QRadioButton *>(W))
  {
    if (Radio->isChecked())
    {
      return false;
    }
    Radio->click();
    return Radio->isChecked();
  }
  if (QComboBox * Combo = qobject_cast<QComboBox *>(W))
  {
    if (Combo->count() < 2)
    {
      return false;
    }
    Combo->setCurrentIndex((Combo->currentIndex() + 1) % Combo->count());
    return true;
  }
  if (QSpinBox * Spin = qobject_cast<QSpinBox *>(W))
  {
    int Value = Spin->value();
    Spin->setValue((Value < Spin->maximum()) ? (Value + 1) : (Value - 1));
    return Spin->value() != Value;
  }
  if (QGroupBox * Group = qobject_cast<QGroupBox *>(W))
  {
    Group->setChecked(!Group->isChecked());
    return true;
  }
  return false;
}

static void TestPreferencesCoverage(const QString & Dir)
{
  std::printf("== Preferences: every option changes WinSCP.ini\n");
  QString IniPath = QDir(Dir).filePath(QStringLiteral("prefs.ini"));
  WriteFile(IniPath, QByteArray());
  Configuration & C = Configuration::Instance();
  C.SetIniFileName(IniPath);
  C.Load();
  C.Save();

  int ControlCount;
  {
    PreferencesDialog Dialog;
    ControlCount = OptionControls(&Dialog).size();
  }
  std::printf("  %d option controls\n", ControlCount);
  QStringList NoEffect;
  int Changed = 0;
  for (int Index = 0; Index < ControlCount; Index++)
  {
    // fresh baseline for every control: the file after OK without changes
    // (Preferences writes some values on OK, e.g. the default workspace name, as WinSCP)
    WriteFile(IniPath, QByteArray());
    C.Load();
    C.Save();
    {
      PreferencesDialog Dialog;
      Dialog.accept();
    }
    QByteArray Before = ReadFile(IniPath);
    QString Caption;
    {
      PreferencesDialog Dialog;
      QList<QWidget *> Controls = OptionControls(&Dialog);
      QWidget * W = Controls.value(Index);
      if (W == nullptr)
      {
        continue;
      }
      Caption = ControlCaption(W);
      // these open dialogs or switch the configuration file
      if (Caption.contains(QStringLiteral("master password")) || Caption.contains(QStringLiteral("INI file")) ||
          Caption.startsWith(QStringLiteral("Automatic")) || Caption.contains(QStringLiteral("Windows registry")))
      {
        continue;
      }
      if (!ChangeControl(W))
      {
        if (Verbose)
        {
          std::printf("  (not changeable alone: %s)\n", qPrintable(Caption));
        }
        continue;
      }
      Changed++;
      Dialog.accept();
    }
    QStringList Diff = IniDiff(Before, ReadFile(IniPath));
    if (Diff.isEmpty())
    {
      NoEffect.append(Caption);
    }
    else if (Verbose)
    {
      std::printf("  %s -> %s\n", qPrintable(Caption), qPrintable(Diff.join(QStringLiteral(", "))));
    }
  }
  std::printf("  %d options changed, %lld without effect on the file\n", Changed, static_cast<long long>(NoEffect.size()));
  // the radio button alone (empty address) means "retrieve from the system" as in WinSCP
  NoEffect.removeAll(QStringLiteral("Use the following external IP address:"));
  for (const QString & Caption : NoEffect)
  {
    std::printf("  NO KEY WRITTEN: %s\n", qPrintable(Caption));
  }
  CHECK(NoEffect.isEmpty());
}

static void TestSiteAdvancedCoverage()
{
  std::printf("== Advanced Site Settings: every option changes the site\n");
  struct Proto { TFSProtocol Protocol; TFtps Ftps; const char * Name; };
  const Proto Protocols[] = {
    { fsSFTP, ftpsNone, "SFTP" }, { fsSCPonly, ftpsNone, "SCP" }, { fsFTP, ftpsExplicitTls, "FTPS" },
    { fsWebDAV, ftpsImplicit, "WebDAVS" }, { fsS3, ftpsImplicit, "S3" } };
  QSet<QString> NoEffect, Effect;
  for (const Proto & P : Protocols)
  {
    SessionData Base;
    Base.Name = QStringLiteral("Site");
    Base.HostName = QStringLiteral("example.com");
    Base.FSProtocol = P.Protocol;
    Base.Ftps = P.Ftps;
    Base.PortNumber = Base.DefaultPort();
    QString BaseDump = SessionDump(Base);
    int ControlCount;
    {
      SessionData D = Base;
      SiteAdvancedDialog Dialog(D);
      ControlCount = OptionControls(&Dialog).size();
    }
    for (int Index = 0; Index < ControlCount; Index++)
    {
      SessionData D = Base;
      QString Caption;
      {
        SiteAdvancedDialog Dialog(D);
        QWidget * W = OptionControls(&Dialog).value(Index);
        if ((W == nullptr) || !W->isVisibleTo(&Dialog) && false)
        {
          continue;
        }
        Caption = ControlCaption(W);
        if (!ChangeControl(W))
        {
          continue;
        }
        Dialog.accept();
      }
      if (SessionDump(D) == BaseDump)
      {
        NoEffect.insert(Caption);
      }
      else
      {
        Effect.insert(Caption);
      }
    }
  }
  NoEffect.subtract(Effect);
  std::printf("  %lld options change the site\n", static_cast<long long>(Effect.size()));
  if (Verbose)
  {
    for (const QString & Caption : Effect)
    {
      std::printf("  ok: %s\n", qPrintable(Caption));
    }
  }
  QStringList List(NoEffect.begin(), NoEffect.end());
  List.sort();
  for (const QString & Caption : List)
  {
    std::printf("  NO SITE CHANGE: %s\n", qPrintable(Caption));
  }
  CHECK(List.isEmpty());
}

//---------------------------------------------------------------------------
// Screenshots of the dialogs of the area
//---------------------------------------------------------------------------
static void TestScreenshots()
{
  if (ScreenshotDir.isEmpty())
  {
    return;
  }
  std::printf("== screenshots to %s\n", qPrintable(ScreenshotDir));
  {
    PreferencesDialog Dialog;
    QTreeWidget * Tree = Dialog.findChild<QTreeWidget *>();
    if (Tree != nullptr)
    {
      int Page = 0;
      for (QTreeWidgetItemIterator It(Tree); *It; ++It, ++Page)
      {
        Tree->setCurrentItem(*It);
        Screenshot(&Dialog, QStringLiteral("prefs_%1_%2").arg(Page, 2, 10, QLatin1Char('0')).arg((*It)->text(0).remove(QLatin1Char(' '))));
      }
    }
  }
  const TFSProtocol Protocols[] = { fsSFTP, fsFTP, fsWebDAV, fsS3, fsSCPonly };
  for (TFSProtocol Protocol : Protocols)
  {
    SessionData D;
    D.FSProtocol = Protocol;
    D.HostName = QStringLiteral("example.com");
    SiteAdvancedDialog Dialog(D);
    QTreeWidget * Tree = Dialog.findChild<QTreeWidget *>();
    if (Tree != nullptr)
    {
      int Page = 0;
      for (QTreeWidgetItemIterator It(Tree); *It; ++It, ++Page)
      {
        Tree->setCurrentItem(*It);
        Screenshot(&Dialog, QStringLiteral("siteadv_%1_%2_%3").arg(static_cast<int>(Protocol)).arg(Page, 2, 10, QLatin1Char('0'))
          .arg((*It)->text(0).remove(QLatin1Char(' '))));
      }
    }
  }
  {
    LoginDialog Dialog(Configuration::Instance().StoredSessions());
    Screenshot(&Dialog, QStringLiteral("login"));
  }
}

//---------------------------------------------------------------------------
// Options added by this verification: Internal editor encoding/colors,
// Panels > File colors (+ the File color dialog), AWS environment credentials
// of the Login dialog, proxy Autodetect and Display Public Key of Advanced
// Site Settings
//---------------------------------------------------------------------------
template<class T>
static T * FindByText(QWidget * Parent, const QString & Text)
{
  for (T * W : Parent->findChildren<T *>())
  {
    if (W->text() == Text)
    {
      return W;
    }
  }
  return nullptr;
}

static void TestAddedOptions(const QString & Dir)
{
  std::printf("== added options\n");
  QString IniPath = QDir(Dir).filePath(QStringLiteral("added.ini"));
  WriteFile(IniPath,
    "[Configuration\\Interface]\n"
    "FileColors=0000FF:*.txt,\"00FF00:*.log | old/\"\n"
    "[Configuration\\Interface\\Editor]\n"
    "Encoding=0\n"
    "FontColor=255\n"
    "BackgroundColor=15794175\n");
  Configuration & C = Configuration::Instance();
  C.SetIniFileName(IniPath);
  C.Load();
  CHECK_EQ(C.EditorEncoding, 0);
  CHECK_EQ(C.EditorFontColor, 255);
  CHECK_EQ(C.EditorBackgroundColor, 15794175);
  {
    PreferencesDialog Dialog;
    Dialog.SelectPage(QStringLiteral("Internal editor"));
    QComboBox * Encoding = nullptr;
    for (QComboBox * Combo : Dialog.findChildren<QComboBox *>())
    {
      if ((Combo->count() == 2) && (Combo->itemText(1) == QStringLiteral("UTF-8")))
      {
        Encoding = Combo;
      }
    }
    CHECK(Encoding != nullptr);
    CHECK(FindByText<QPushButton>(&Dialog, QStringLiteral("&Text color")) != nullptr);
    CHECK(FindByText<QPushButton>(&Dialog, QStringLiteral("Default &background")) != nullptr);
    if (Encoding != nullptr)
    {
      CHECK_EQ(Encoding->currentIndex(), 0);
    }
    Screenshot(&Dialog, QStringLiteral("added_internal_editor"));
    // File colors page
    Dialog.SelectPage(QStringLiteral("File colors"));
    QTreeWidget * Colors = nullptr;
    for (QTreeWidget * Tree : Dialog.findChildren<QTreeWidget *>())
    {
      if ((Tree->topLevelItemCount() == 2) && (Tree->topLevelItem(0)->text(0) == QStringLiteral("*.txt")))
      {
        Colors = Tree;
      }
    }
    CHECK(Colors != nullptr);
    if (Colors != nullptr)
    {
      CHECK_EQ(Colors->topLevelItem(1)->text(0), QStringLiteral("*.log | old/"));
      CHECK(Colors->topLevelItem(0)->foreground(0).color() == QColor(255, 0, 0));
      Screenshot(&Dialog, QStringLiteral("added_file_colors"));
      // Add... opens the File color dialog: set a mask and accept it
      Colors->setCurrentItem(nullptr);
      QPushButton * Add = FindByText<QPushButton>(Colors->parentWidget(), QStringLiteral("&Add..."));
      CHECK(Add != nullptr);
      bool Seen = false;
      ModalHandler = [&Seen](QWidget * Modal)
      {
        QDialog * D = qobject_cast<QDialog *>(Modal);
        if ((D == nullptr) || (D->windowTitle() != QStringLiteral("File color")))
        {
          return false;
        }
        Seen = true;
        Screenshot(D, QStringLiteral("added_file_color_dialog"));
        QComboBox * Mask = D->findChild<QComboBox *>();
        Mask->setEditText(QStringLiteral("*.bak"));
        D->accept();
        return true;
      };
      if (Add != nullptr)
      {
        Add->click();
      }
      ModalHandler = nullptr;
      CHECK(Seen);
      CHECK_EQ(Colors->topLevelItemCount(), 3);
      // remove the first one
      Colors->setCurrentItem(Colors->topLevelItem(0));
      QPushButton * Remove = FindByText<QPushButton>(Colors->parentWidget(), QStringLiteral("&Remove"));
      CHECK(Remove && Remove->isEnabled());
      if (Remove != nullptr)
      {
        Remove->click();
      }
      CHECK_EQ(Colors->topLevelItemCount(), 2);
    }
    if (Encoding != nullptr)
    {
      Encoding->setCurrentIndex(1);
    }
    Dialog.accept();
  }
  IniFile Saved;
  Saved.Load(IniPath);
  CHECK_EQ(Saved.ReadRaw(QStringLiteral("Configuration\\Interface\\Editor"), QStringLiteral("Encoding")), QStringLiteral("65001"));
  CHECK_EQ(Saved.ReadRaw(QStringLiteral("Configuration\\Interface\\Editor"), QStringLiteral("FontColor")), QStringLiteral("255"));
  CHECK_EQ(Saved.ReadRaw(QStringLiteral("Configuration\\Interface\\Editor"), QStringLiteral("BackgroundColor")), QStringLiteral("15794175"));
  QString FileColors = UnMungeStr(Saved.ReadRaw(QStringLiteral("Configuration\\Interface"), QStringLiteral("FileColors")));
  std::printf("  FileColors=%s\n", qPrintable(FileColors));
  CHECK(FileColors.contains(QStringLiteral("*.log | old/")));
  CHECK(FileColors.contains(QStringLiteral(":*.bak")));
  CHECK(!FileColors.contains(QStringLiteral("*.txt")));

  // Login dialog: Credentials from AWS environment
  {
    qputenv("AWS_ACCESS_KEY_ID", "AKIAVERIFYTEST");
    qputenv("AWS_SECRET_ACCESS_KEY", "verify-secret");
    StoredSessionList Sites;
    LoginDialog Dialog(Sites);
    SessionData D;
    D.FSProtocol = fsS3;
    D.Ftps = ftpsImplicit;
    D.HostName = QStringLiteral("s3.amazonaws.com");
    D.PortNumber = 443;
    Dialog.SetInitialSession(D);
    Dialog.show();
    QApplication::processEvents();
    QCheckBox * Env = FindByText<QCheckBox>(&Dialog, QStringLiteral("&Credentials from AWS environment:"));
    CHECK(Env != nullptr);
    if (Env != nullptr)
    {
      CHECK(Env->isVisible());
      Env->click();
      QApplication::processEvents();
      CHECK(Env->isChecked());
      bool Found = false;
      for (QLineEdit * Edit : Dialog.findChildren<QLineEdit *>())
      {
        if (Edit->text() == QStringLiteral("AKIAVERIFYTEST"))
        {
          Found = true;
          CHECK(!Edit->isEnabled());
        }
      }
      CHECK(Found);
      Screenshot(&Dialog, QStringLiteral("added_login_s3_env"));
    }
    qunsetenv("AWS_ACCESS_KEY_ID");
    qunsetenv("AWS_SECRET_ACCESS_KEY");
  }

  // Login dialog: Agent Access... next to Advanced, its choice kept in the site
  {
    StoredSessionList Sites;
    LoginDialog Dialog(Sites);
    SessionData D;
    D.FSProtocol = fsSFTP;
    D.HostName = QStringLiteral("agent.verify.test");
    D.UserName = QStringLiteral("deploy");
    Dialog.SetInitialSession(D);
    Dialog.show();
    QApplication::processEvents();
    QPushButton * Agent = FindByText<QPushButton>(&Dialog, QStringLiteral("A&gent Access..."));
    QToolButton * Advanced = nullptr;
    for (QToolButton * B : Dialog.findChildren<QToolButton *>())
    {
      if (B->text() == QStringLiteral("A&dvanced..."))
      {
        Advanced = B;
      }
    }
    CHECK((Agent != nullptr) && (Advanced != nullptr));
    if ((Agent != nullptr) && (Advanced != nullptr))
    {
      CHECK(Agent->isVisible() && Agent->isEnabled());
      // directly left of Advanced
      CHECK(Agent->mapTo(&Dialog, QPoint(0, 0)).x() < Advanced->mapTo(&Dialog, QPoint(0, 0)).x());
      CHECK(qAbs(Agent->mapTo(&Dialog, QPoint(0, 0)).y() - Advanced->mapTo(&Dialog, QPoint(0, 0)).y()) < 10);
      Screenshot(&Dialog, QStringLiteral("added_login_agent_button"));
      // the next modal dialog (instead of the modal killer)
      ModalHandler = [](QWidget * Modal) -> bool
      {
        if (!Modal->windowTitle().startsWith(QStringLiteral("Agent Access")))
        {
          return false;
        }
        ModalHandler = nullptr;
        auto Combos = Modal->findChildren<QComboBox *>();
        CHECK(Combos.size() == 1);
        // the configuration is not encrypted here: turning access on is not possible yet
        CHECK(!Combos.value(0)->isEnabled());
        Combos.value(0)->setEnabled(true);
        Combos.value(0)->setCurrentIndex(2);
        QCheckBox * Files = FindByText<QCheckBox>(Modal, QStringLiteral("&Download files"));
        CHECK(Files != nullptr);
        if (Files != nullptr) Files->setChecked(false);
        Screenshot(Modal, QStringLiteral("added_agent_access_dialog"));
        qobject_cast<QDialog *>(Modal)->accept();
        return true;
      };
      Agent->click();
      QApplication::processEvents();
      QAction * Login = nullptr;
      for (QAction * A : Dialog.findChildren<QAction *>())
      {
        if (A->text() == QStringLiteral("Login"))
        {
          Login = A;
        }
      }
      CHECK(Login != nullptr);
      if (Login != nullptr)
      {
        Login->trigger();
        QApplication::processEvents();
        auto Opened = Dialog.SessionToOpen();
        CHECK(Opened != nullptr);
        if (Opened != nullptr)
        {
          CHECK_EQ(Opened->AgentAccess, 2);
          CHECK_EQ(Opened->AgentPermissions & 0x03, 0x01);
        }
      }
    }
  }

  // Advanced Site Settings: proxy Autodetect
  {
    QByteArray OldProxy = qgetenv("http_proxy");
    qputenv("http_proxy", "http://proxy.verify.test:3128");
    SessionData D;
    D.HostName = QStringLiteral("example.com");
    D.FSProtocol = fsSFTP;
    {
      SiteAdvancedDialog Dialog(D);
      Dialog.SelectPage(QStringLiteral("Proxy"));
      QPushButton * Autodetect = FindByText<QPushButton>(&Dialog, QStringLiteral("&Autodetect"));
      CHECK(Autodetect != nullptr);
      if (Autodetect != nullptr)
      {
        Autodetect->click();
      }
      Screenshot(&Dialog, QStringLiteral("added_proxy_autodetect"));
      Dialog.accept();
    }
    CHECK_EQ(static_cast<int>(D.ProxyMethod), static_cast<int>(pmHTTP));
    CHECK_EQ(D.ProxyHost, QStringLiteral("proxy.verify.test"));
    CHECK_EQ(D.ProxyPort, 3128);
    if (OldProxy.isEmpty())
    {
      qunsetenv("http_proxy");
    }
    else
    {
      qputenv("http_proxy", OldProxy);
    }
  }

  // Advanced Site Settings: Display Public Key (key generated in a temporary directory)
  {
    QString KeyFile = QDir(Dir).filePath(QStringLiteral("verify_key"));
    int Exit = QProcess::execute(QStringLiteral("ssh-keygen"),
      {QStringLiteral("-q"), QStringLiteral("-t"), QStringLiteral("ed25519"), QStringLiteral("-N"), QString(),
       QStringLiteral("-C"), QStringLiteral("verify@nixscp"), QStringLiteral("-f"), KeyFile});
    if (Exit != 0)
    {
      std::printf("  SKIP Display Public Key: ssh-keygen not available\n");
    }
    else
    {
      SessionData D;
      D.HostName = QStringLiteral("example.com");
      D.FSProtocol = fsSFTP;
      D.PublicKeyFile = KeyFile;
      SiteAdvancedDialog Dialog(D);
      Dialog.SelectPage(QStringLiteral("Authentication"));
      QPushButton * View = FindByText<QPushButton>(&Dialog, QStringLiteral("&Display Public Key"));
      CHECK(View != nullptr);
      CHECK(FindByText<QPushButton>(&Dialog, QStringLiteral("&Tools")) != nullptr);
      Screenshot(&Dialog, QStringLiteral("added_authentication"));
      QString Shown;
      ModalHandler = [&Shown](QWidget * Modal)
      {
        QMessageBox * Box = qobject_cast<QMessageBox *>(Modal);
        if (Box == nullptr)
        {
          return false;
        }
        Shown = Box->text() + QLatin1Char('\n') + Box->informativeText();
        Box->accept();
        return true;
      };
      if (View != nullptr)
      {
        View->click();
      }
      ModalHandler = nullptr;
      std::printf("  Display Public Key: %s\n", qPrintable(Shown.left(120)));
      CHECK(Shown.contains(QStringLiteral("authorized_keys")));
      CHECK(Shown.contains(QStringLiteral("ssh-ed25519 ")));
      CHECK(Shown.contains(QStringLiteral("verify@nixscp")));
    }
  }
  C.SetIniFileName(QDir(Dir).filePath(QStringLiteral("WinSCP.ini")));
  C.Load();
}

// F-12, B21, M7, S-xx: Site Advanced shows what works, and keeps what is stored
static void TestSiteAdvancedControls()
{
  std::printf("[Site Advanced: controls]\n");
  auto Texts = [](QWidget * Dialog)
  {
    QStringList Result;
    for (QLabel * L : Dialog->findChildren<QLabel *>())
    {
      Result.append(L->text());
    }
    for (QAbstractButton * B : Dialog->findChildren<QAbstractButton *>())
    {
      Result.append(B->text());
    }
    for (QGroupBox * G : Dialog->findChildren<QGroupBox *>())
    {
      Result.append(G->title());
    }
    return Result.join(QLatin1Char('\n'));
  };
  SessionData Stored;
  Stored.HostName = QStringLiteral("example.com");
  Stored.SFTPMaxVersion = 4;
  Stored.RekeyTime = 33;
  Stored.AuthGSSAPIKEX = true;
  Stored.Ssh2DES = true;
  Stored.DSTMode = dstmWin;
  Stored.TunnelLocalPortNumber = 50010;
  Stored.Bug[sbHMAC2] = asOn;
  Stored.Bug[sbRekey2] = asOn;
  {
    SessionData D = Stored;
    SiteAdvancedDialog Dialog(D);
    QString All = Texts(&Dialog);
    for (const char * Gone : {"Preferred SFTP protocol version", "single-&DES", "GSSAPI key exchange", "Max &minutes before rekey",
           "Daylight saving time", "Local tunnel port", "Miscomputes", "winadj", "Ignores SSH ma"})
    {
      CHECK(!All.contains(QString::fromLatin1(Gone)));
    }
    CHECK(All.contains(QStringLiteral("3 (the only version NixSCP uses)")));
    CHECK(All.contains(QStringLiteral("Handles SSH &key re-exchange badly")));
    Dialog.accept();
    CHECK_EQ(D.SFTPMaxVersion, 4);
    CHECK_EQ(D.RekeyTime, 33);
    CHECK(D.AuthGSSAPIKEX);
    CHECK(D.Ssh2DES);
    CHECK(D.DSTMode == dstmWin);
    CHECK_EQ(D.TunnelLocalPortNumber, 50010);
    CHECK(D.Bug[sbHMAC2] == asOn);
    CHECK(D.Bug[sbRekey2] == asOn);
  }
  // F-12: the logon types of an FTP proxy
  auto ProxyCombo = [](QWidget * Dialog) -> QComboBox *
  {
    for (QComboBox * Combo : Dialog->findChildren<QComboBox *>())
    {
      if ((Combo->count() >= 4) && (Combo->itemText(1) == QStringLiteral("SOCKS4")))
      {
        return Combo;
      }
    }
    return nullptr;
  };
  SessionData Ftp;
  Ftp.HostName = QStringLiteral("example.com");
  Ftp.FSProtocol = fsFTP;
  Ftp.PortNumber = 21;
  {
    SessionData D = Ftp;
    D.FtpProxyLogonType = 3;
    SiteAdvancedDialog Dialog(D);
    QComboBox * Combo = ProxyCombo(&Dialog);
    CHECK(Combo != nullptr);
    CHECK_EQ(Combo->count(), 12);
    CHECK_EQ(Combo->currentText(), QStringLiteral("OPEN %host"));
    Combo->setCurrentIndex(Combo->findText(QStringLiteral("USER %user@%host %proxyuser")));
    Dialog.accept();
    CHECK_EQ(D.FtpProxyLogonType, 7);
    CHECK(D.ProxyMethod == pmNone);
  }
  {
    SessionData D = Ftp;
    D.FtpProxyLogonType = 3;
    SiteAdvancedDialog Dialog(D);
    QComboBox * Combo = ProxyCombo(&Dialog);
    Combo->setCurrentIndex(pmSocks5);
    Dialog.accept();
    CHECK_EQ(D.FtpProxyLogonType, 0);
    CHECK(D.ProxyMethod == pmSocks5);
  }
  {
    // nothing of the kind for other protocols, which have no logon
    SessionData D = Stored;
    SiteAdvancedDialog Dialog(D);
    CHECK(ProxyCombo(&Dialog) != nullptr);
    CHECK_EQ(ProxyCombo(&Dialog)->findText(QStringLiteral("OPEN %host")), -1);
  }
}

int main(int argc, char ** argv)
{
  QTemporaryDir ConfigDir;
  qputenv("XDG_CONFIG_HOME", ConfigDir.path().toLocal8Bit());
  QApplication App(argc, argv);
  TestSiteAdvancedControls();
  ScreenshotDir = qEnvironmentVariable("NIXSCP_TEST_SCREENSHOT_DIR");
  Verbose = !qEnvironmentVariableIsEmpty("NIXSCP_VERIFY_VERBOSE");
  InstallModalKiller();
  QTemporaryDir Dir;
  Configuration::Instance().SetSystemIniFileNames(QStringList());

  TestIniRoundTrip(Dir.path());
  TestRegImport(Dir.path());
  TestPreferencesCoverage(Dir.path());
  TestSiteAdvancedCoverage();
  TestAddedOptions(Dir.path());
  TestScreenshots();

  std::printf("%s (%d failures)\n", Failures ? "FAILED" : "OK", Failures);
  return Failures ? 1 : 0;
}

#endif
