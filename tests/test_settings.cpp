// Settings tests: WinSCP.ini compatible load/save of all site settings
// (core/SessionData.cpp) and preferences (core/Configuration.cpp)
#include "core/Configuration.h"
#include "core/IniStorage.h"
#include "core/Security.h"
#include "core/SessionData.h"

#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <cmath>
#include <cstdio>

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); Failures++; } } while (0)

static void WriteFile(const QString & Path, const QByteArray & Data)
{
  QFile F(Path);
  CHECK(F.open(QIODevice::WriteOnly));
  F.write(Data);
}

// Site written by WinSCP (only non-default values are present, as
// TSessionData::Save writes relative to Default Settings)
static QByteArray WinSCPIni()
{
  QByteArray Password = Security::EncryptPassword(QStringLiteral("sitepw"), QStringLiteral("userexample.com"));
  QByteArray ProxyPassword = Security::EncryptPassword(QStringLiteral("proxypw"), QStringLiteral("puserproxy.example.com"));
  QByteArray TunnelPassword = Security::EncryptPassword(QStringLiteral("tunnelpw"), QStringLiteral("juserjump.example.com"));
  return
    "[Sessions\\Default%20Settings]\r\n"
    "Compression=1\r\n"
    "Timeout=20\r\n"
    "\r\n"
    "[Sessions\\Work/Full%20Site]\r\n"
    "HostName=example.com\r\n"
    "PortNumber=2222\r\n"
    "PingType=2\r\n"
    "PingInterval=1\r\n"
    "PingIntervalSecs=15\r\n"
    "Timeout=30\r\n"
    "TryAgent=0\r\n"
    "AgentFwd=1\r\n"
    "AuthKI=0\r\n"
    "AuthKIPassword=0\r\n"
    "SshHostKey=ssh-ed25519%20255%20SHA256:abc\r\n"
    "Note=First%20line\r\n"
    "AuthGSSAPI=0\r\n"
    "GSSAPIFwdTGT=1\r\n"
    "Compression=0\r\n"
    "Ssh2DES=1\r\n"
    "SshNoUserAuth=1\r\n"
    "Cipher=chacha20,aes,aesgcm,WARN,3des,des,blowfish,arcfour\r\n"
    "KEX=ecdh,dh-gex-sha1,WARN,rsa\r\n"
    "HostKey=ed25519,rsa,WARN,ecdsa,dsa,ed448\r\n"
    "AddressFamily=1\r\n"
    "RekeyBytes=512M\r\n"
    "RekeyTime=30\r\n"
    "TcpNoDelay=1\r\n"
    "UserName=user\r\n"
    "PublicKeyFile=/home/user/.ssh/id_ed25519.ppk\r\n"
    "DetachedCertificate=/home/user/.ssh/id-cert.pub\r\n"
    "FSProtocol=0\r\n"
    "LocalDirectory=/home/user/work\r\n"
    "RemoteDirectory=/var/www\r\n"
    "SynchronizeBrowsing=1\r\n"
    "UpdateDirectories=0\r\n"
    "CacheDirectories=0\r\n"
    "CacheDirectoryChanges=0\r\n"
    "PreserveDirectoryChanges=0\r\n"
    "ResolveSymlinks=0\r\n"
    "FollowDirectorySymlinks=1\r\n"
    "ConsiderDST=0\r\n"
    "Shell=/bin/bash\r\n"
    "ClearAliases=0\r\n"
    "UnsetNationalVars=0\r\n"
    "ListingCommand=ls%20-gla\r\n"
    "IgnoreLsWarnings=0\r\n"
    "SCPLsFullTime=1\r\n"
    "Scp1Compatibility=1\r\n"
    "TimeDifference=555555555555B53F\r\n"
    "TimeDifferenceAuto=0\r\n"
    "DeleteToRecycleBin=1\r\n"
    "OverwrittenToRecycleBin=1\r\n"
    "RecycleBinPath=/home/user/.trash\r\n"
    "PostLoginCommands=cd%20/tmp\r\n"
    "ReturnVar=status\r\n"
    "ExitCode1IsError=1\r\n"
    "LookupUserGroups2=1\r\n"
    "EOLType=1\r\n"
    "TrimVMSVersions=1\r\n"
    "Utf=0\r\n"
    "SendBuf=0\r\n"
    "SourceAddress=10.0.0.1\r\n"
    "SshSimple=0\r\n"
    "ProxyMethod=2\r\n"
    "ProxyHost=proxy.example.com\r\n"
    "ProxyPort=1080\r\n"
    "ProxyUsername=puser\r\n"
    "ProxyTelnetCommand=connect %host %port\\n\\n\r\n"  // StringRaw: not munged
    "ProxyDNS=2\r\n"
    "ProxyLocalhost=1\r\n"
    "BugHMAC2=1\r\n"
    "BugWinAdj=2\r\n"
    "BugRekey2=0\r\n"
    "SftpServer=sudo%20su%20-c%20/usr/lib/sftp-server\r\n"
    "SFTPSymlinkBug=0\r\n"
    "SFTPMaxVersion=3\r\n"
    "SFTPDownloadQueue=16\r\n"
    "SFTPRealPath=1\r\n"
    "UsePosixRename=1\r\n"
    "Color=255\r\n"
    "Tunnel=1\r\n"
    "TunnelHostName=jump.example.com\r\n"
    "TunnelPortNumber=2200\r\n"
    "TunnelUserName=juser\r\n"
    "TunnelPublicKeyFile=/home/user/.ssh/jump.ppk\r\n"
    "TunnelLocalPortNumber=50010\r\n"
    "FtpPasvMode=0\r\n"
    "FtpPingType=3\r\n"
    "FtpPingInterval=45\r\n"
    "FtpListAll=1\r\n"
    "MinTlsVersion=13\r\n"
    "SslSessionReuse=0\r\n"
    "S3DefaultRegion=eu-west-1\r\n"
    "S3UrlStyle=1\r\n"
    "UnknownFutureKey=keepme\r\n"
    "Password=" + Password + "\r\n"
    "ProxyPasswordEnc=" + ProxyPassword + "\r\n"
    "TunnelPassword=" + TunnelPassword + "\r\n"
    "\r\n"
    "[Sessions\\Named%20Enums]\r\n"
    "HostName=other.example.com\r\n"
    "ProxyMethod=cmd\r\n"
    "ProxyTelnetCommand=ssh -W %host:%port gw\r\n"
    "SCPLsFullTime=off\r\n"
    "FtpUseMlsd=on\r\n"
    "Utf=on\r\n"
    "BuggyMAC=1\r\n"
    "PingIntervalSecs=0\r\n"
    "PingInterval=0\r\n"
    "\r\n";
}

static void CheckFullSite(const SessionData & D)
{
  CHECK(D.HostName == QStringLiteral("example.com"));
  CHECK(D.PortNumber == 2222);
  CHECK(D.UserName == QStringLiteral("user"));
  CHECK(D.Password == QStringLiteral("sitepw"));
  CHECK(D.FSProtocol == fsSCPonly);
  CHECK(D.PingType == ptDummyCommand);
  CHECK(D.PingInterval == 75);
  CHECK(D.Timeout == 30);
  CHECK(!D.TryAgent);
  CHECK(D.AgentFwd);
  CHECK(!D.AuthKI);
  CHECK(!D.AuthKIPassword);
  CHECK(D.HostKey == QStringLiteral("ssh-ed25519 255 SHA256:abc"));
  CHECK(D.Note == QStringLiteral("First line"));
  CHECK(!D.AuthGSSAPI);
  CHECK(D.GSSAPIFwdTGT);
  CHECK(!D.Compression);
  CHECK(D.Ssh2DES);
  CHECK(D.SshNoUserAuth);
  CHECK(D.CipherList == QStringLiteral("chacha20,aes,aesgcm,WARN,3des,des,blowfish,arcfour"));
  CHECK(D.Ciphers().first() == QStringLiteral("chacha20"));
  CHECK(D.Ciphers().size() == SessionData::CipherNames().size());
  // Missing algorithms get merged (kex list in file is partial)
  CHECK(D.Kexes().size() == SessionData::KexNames().size());
  CHECK(D.Kexes().indexOf(QStringLiteral("ecdh")) < D.Kexes().indexOf(QStringLiteral("WARN")));
  CHECK(D.Kexes().indexOf(QStringLiteral("dh-group1-sha1")) > D.Kexes().indexOf(QStringLiteral("WARN")));
  CHECK(D.HostKeys().first() == QStringLiteral("ed25519"));
  CHECK(D.AddressFamily == afIPv4);
  CHECK(D.RekeyData == QStringLiteral("512M"));
  CHECK(D.RekeyBytes() == 512LL * 1024 * 1024);
  CHECK(D.RekeyTime == 30);
  CHECK(D.TcpNoDelay);
  CHECK(D.PublicKeyFile == QStringLiteral("/home/user/.ssh/id_ed25519.ppk"));
  CHECK(D.DetachedCertificate == QStringLiteral("/home/user/.ssh/id-cert.pub"));
  CHECK(D.LocalDirectory == QStringLiteral("/home/user/work"));
  CHECK(D.RemoteDirectory == QStringLiteral("/var/www"));
  CHECK(D.SynchronizeBrowsing);
  CHECK(!D.UpdateDirectories);
  CHECK(!D.CacheDirectories);
  CHECK(!D.CacheDirectoryChanges);
  CHECK(!D.PreserveDirectoryChanges);
  CHECK(!D.ResolveSymlinks);
  CHECK(D.FollowDirectorySymlinks);
  CHECK(D.DSTMode == dstmWin);
  CHECK(D.Shell == QStringLiteral("/bin/bash"));
  CHECK(!D.ClearAliases);
  CHECK(!D.UnsetNationalVars);
  CHECK(D.ListingCommand == QStringLiteral("ls -gla"));
  CHECK(!D.IgnoreLsWarnings);
  CHECK(D.SCPLsFullTime == asOff);
  CHECK(D.Scp1Compatibility);
  CHECK(std::fabs(D.TimeDifference - 2.0 / 24) < 1e-12);
  CHECK(!D.TimeDifferenceAuto);
  CHECK(D.DeleteToRecycleBin);
  CHECK(D.OverwrittenToRecycleBin);
  CHECK(D.RecycleBinPath == QStringLiteral("/home/user/.trash"));
  CHECK(D.PostLoginCommands == QStringLiteral("cd /tmp"));
  CHECK(D.ReturnVar == QStringLiteral("status"));
  CHECK(D.ExitCode1IsError);
  CHECK(D.LookupUserGroups == asOff);
  CHECK(D.EOLType == eolCRLF);
  CHECK(D.TrimVMSVersions);
  CHECK(D.NotUtf == asOn);
  CHECK(D.SendBuf == 0);
  CHECK(D.SourceAddress == QStringLiteral("10.0.0.1"));
  CHECK(!D.SshSimple);
  CHECK(D.ProxyMethod == pmSocks5);
  CHECK(D.ProxyHost == QStringLiteral("proxy.example.com"));
  CHECK(D.ProxyPort == 1080);
  CHECK(D.ProxyUsername == QStringLiteral("puser"));
  CHECK(D.ProxyPassword == QStringLiteral("proxypw"));
  CHECK(D.ProxyTelnetCommand == QStringLiteral("connect %host %port\\n\\n"));
  CHECK(D.ProxyCommand() == D.ProxyTelnetCommand);
  CHECK(D.ProxyDNS == asOn);
  CHECK(D.ProxyLocalhost);
  CHECK(D.Bug[sbHMAC2] == asOff);
  CHECK(D.Bug[sbWinAdj] == asOn);
  CHECK(D.Bug[sbRekey2] == asAuto);
  CHECK(D.Bug[sbChanReq] == asAuto);
  CHECK(D.SftpServer == QStringLiteral("sudo su -c /usr/lib/sftp-server"));
  CHECK(D.SFTPBug[sbSymlink] == asOn);
  CHECK(D.SFTPBug[sbSignedTS] == asAuto);
  CHECK(D.SFTPMaxVersion == 3);
  CHECK(D.SFTPDownloadQueue == 16);
  CHECK(D.SFTPUploadQueue == 64);
  CHECK(D.SFTPRealPath == asOff);
  CHECK(D.UsePosixRename);
  CHECK(D.Color == 255);
  CHECK(D.Tunnel);
  CHECK(D.TunnelHostName == QStringLiteral("jump.example.com"));
  CHECK(D.TunnelPortNumber == 2200);
  CHECK(D.TunnelUserName == QStringLiteral("juser"));
  CHECK(D.TunnelPassword == QStringLiteral("tunnelpw"));
  CHECK(D.TunnelPublicKeyFile == QStringLiteral("/home/user/.ssh/jump.ppk"));
  CHECK(D.TunnelLocalPortNumber == 50010);
  CHECK(!D.FtpPasvMode);
  CHECK(D.FtpPingType == fptDirectoryListing);
  CHECK(D.FtpPingInterval == 45);
  CHECK(D.FtpListAll == asOff);
  CHECK(D.MinTlsVersion == tls13);
  CHECK(D.MaxTlsVersion == tls13);
  CHECK(!D.SslSessionReuse);
  CHECK(D.S3DefaultRegion == QStringLiteral("eu-west-1"));
  CHECK(D.S3UrlStyle == s3usPath);
}

static void TestSessionSettings()
{
  QTemporaryDir Dir;
  QString IniPath = Dir.filePath(QStringLiteral("WinSCP.ini"));
  WriteFile(IniPath, WinSCPIni());

  IniFile File;
  CHECK(File.Load(IniPath));
  StoredSessionList List;
  List.Load(File);
  CHECK(List.DefaultSettings().Compression);
  CHECK(List.DefaultSettings().Timeout == 20);

  auto Full = List.FindByName(QStringLiteral("Work/Full Site"));
  CHECK(Full != nullptr);
  if (Full)
  {
    CheckFullSite(*Full);
  }

  auto Named = List.FindByName(QStringLiteral("Named Enums"));
  CHECK(Named != nullptr);
  if (Named)
  {
    // Inherited from Default Settings
    CHECK(Named->Compression);
    CHECK(Named->Timeout == 20);
    // Factory defaults
    CHECK(Named->PortNumber == 22);
    CHECK(Named->FSProtocol == fsSFTP);
    CHECK(Named->CipherList == SessionData::DefaultCipherList());
    CHECK(Named->ProxyHost == QStringLiteral("proxy"));
    CHECK(Named->ProxyPort == 80);
    CHECK(Named->RekeyBytes() == 1024LL * 1024 * 1024);
    CHECK(Named->RekeyTime == 60);
    CHECK(Named->ListingCommand == QStringLiteral("ls -la"));
    CHECK(Named->FtpPingType == fptDummyCommand);
    CHECK(Named->SFTPMaxVersion == SFTPMaxVersionAuto);
    CHECK(Named->TimeDifferenceAuto);
    // Names instead of numbers (TIntMapping)
    CHECK(Named->ProxyMethod == pmCmd);
    CHECK(Named->ProxyLocalCommand == QStringLiteral("ssh -W %host:%port gw"));
    CHECK(Named->ProxyTelnetCommand == QStringLiteral("connect %host %port\\n"));
    CHECK(Named->ProxyCommand() == Named->ProxyLocalCommand);
    CHECK(Named->SCPLsFullTime == asOff);
    CHECK(Named->FtpUseMlsd == asOn);
    CHECK(Named->NotUtf == asOff);  // AutoSwitchReversedMapping
    CHECK(Named->Bug[sbHMAC2] == asOn);  // legacy BuggyMAC
    CHECK(Named->PingInterval == 30);  // 0 means default
  }

  // Save and reload
  IniFile Out;
  List.Save(Out);
  QString OutPath = Dir.filePath(QStringLiteral("nixscp.ini"));
  CHECK(Out.Save(OutPath));
  // Site keys equal to Default Settings are not written, as in WinSCP
  CHECK(Out.ReadRaw(QStringLiteral("Sessions\\Named%20Enums"), QStringLiteral("Compression"), QStringLiteral("absent")) == QStringLiteral("absent"));
  CHECK(Out.ReadRaw(QStringLiteral("Sessions\\Named%20Enums"), QStringLiteral("ProxyMethod")) == QStringLiteral("5"));
  CHECK(Out.ReadRaw(QStringLiteral("Sessions\\Default%20Settings"), QStringLiteral("Compression")) == QStringLiteral("1"));
  CHECK(Out.ReadRaw(QStringLiteral("Sessions\\Work/Full%20Site"), QStringLiteral("UnknownFutureKey")) == QStringLiteral("keepme"));
  CHECK(Out.ReadRaw(QStringLiteral("Sessions\\Work/Full%20Site"), QStringLiteral("TimeDifference")) == QStringLiteral("555555555555B53F"));
  CHECK(Out.ReadRaw(QStringLiteral("Sessions\\Work/Full%20Site"), QStringLiteral("ProxyDNS")) == QStringLiteral("2"));
  CHECK(Out.ReadRaw(QStringLiteral("Sessions\\Work/Full%20Site"), QStringLiteral("BugHMAC2")) == QStringLiteral("1"));
  CHECK(Out.ReadRaw(QStringLiteral("Sessions\\Work/Full%20Site"), QStringLiteral("PingInterval")) == QStringLiteral("1"));
  CHECK(Out.ReadRaw(QStringLiteral("Sessions\\Work/Full%20Site"), QStringLiteral("PingIntervalSecs")) == QStringLiteral("15"));
  CHECK(!Out.ReadRaw(QStringLiteral("Sessions\\Work/Full%20Site"), QStringLiteral("ProxyPasswordEnc")).isEmpty());

  IniFile In;
  CHECK(In.Load(OutPath));
  StoredSessionList Reloaded;
  Reloaded.Load(In);
  auto Full2 = Reloaded.FindByName(QStringLiteral("Work/Full Site"));
  CHECK(Full2 != nullptr);
  if (Full2)
  {
    CheckFullSite(*Full2);
  }
  auto Named2 = Reloaded.FindByName(QStringLiteral("Named Enums"));
  CHECK(Named2 != nullptr);
  if (Named2)
  {
    CHECK(Named2->Compression);
    CHECK(Named2->ProxyMethod == pmCmd);
    CHECK(Named2->ProxyLocalCommand == QStringLiteral("ssh -W %host:%port gw"));
    CHECK(Named2->NotUtf == asOff);
    CHECK(Named2->Bug[sbHMAC2] == asOn);
  }

  // Full save (no Default) of a modified site round trips every field
  SessionData Data;
  Data.HostName = QStringLiteral("h");
  Data.ProxyMethod = pmHTTP;
  Data.ProxyPassword = QStringLiteral("p");
  Data.Bug[sbChanReq] = asOff;
  Data.SFTPBug[sbSignedTS] = asOn;
  Data.TimeDifference = -0.5 / 24;
  Data.PingInterval = 125;
  IniFile Single;
  {
    IniStorage S(&Single);
    S.OpenSubKey(QStringLiteral("X"), true);
    Data.Save(S);
  }
  SessionData Loaded;
  {
    IniStorage S(&Single);
    S.OpenSubKey(QStringLiteral("X"), false);
    Loaded.Load(S);
  }
  CHECK(Loaded.ProxyMethod == pmHTTP);
  CHECK(Loaded.ProxyPassword == QStringLiteral("p"));
  CHECK(Loaded.Bug[sbChanReq] == asOff);
  CHECK(Loaded.SFTPBug[sbSignedTS] == asOn);
  CHECK(std::fabs(Loaded.TimeDifference + 0.5 / 24) < 1e-12);
  CHECK(Loaded.PingInterval == 125);
  CHECK(Single.ReadRaw(QStringLiteral("X"), QStringLiteral("PingInterval")) == QStringLiteral("2"));
  CHECK(Single.ReadRaw(QStringLiteral("X"), QStringLiteral("PingIntervalSecs")) == QStringLiteral("5"));
}

static void TestAlgoList()
{
  // Like TSessionData::SetAlgoList: unknown names dropped, missing appended
  QStringList L = SessionData::NormalizeAlgoList(QStringLiteral("aes,bogus,aes,WARN"), SessionData::DefaultCipherList());
  CHECK(L.size() == SessionData::CipherNames().size());
  CHECK(L.first() == QStringLiteral("aes"));
  CHECK(L.count(QStringLiteral("aes")) == 1);
  CHECK(!L.contains(QStringLiteral("bogus")));
  CHECK(SessionData::NormalizeAlgoList(SessionData::DefaultKexList(), SessionData::DefaultKexList()).join(QLatin1Char(',')) ==
        SessionData::DefaultKexList());
  // Items prioritized before the first custom item stay at their default position
  QStringList H = SessionData::NormalizeAlgoList(QStringLiteral("rsa"), SessionData::DefaultHostKeyList());
  CHECK(H.size() == 6);
  CHECK(H.indexOf(QStringLiteral("ed448")) == 0);
}

static void TestConfiguration()
{
  QTemporaryDir Dir;
  QString IniPath = Dir.filePath(QStringLiteral("nixscp.ini"));
  WriteFile(IniPath,
    "[Configuration\\Interface]\r\n"
    "CopyOnDoubleClick=1\r\n"
    "QueueTransfersLimit=5\r\n"
    "ConfirmOverwriting=0\r\n"
    "ShowHiddenFiles=1\r\n"
    "FormatSizeBytes=2\r\n"
    "PanelSearch=1\r\n"
    "BeepOnFinishAfter=176CC1166CC1463F\r\n"
    "RefreshRemotePanelInterval=176CC1166CC1563F\r\n"
    "DDTransferConfirmation2=1\r\n"
    "QueueRememberPassword=0\r\n"
    "SessionReopenAuto=0\r\n"
    "PathInCaption=1\r\n"
    "ShowLoginWhenNoSession=0\r\n"
    "\r\n"
    "[Configuration\\Interface\\CopyParam]\r\n"
    "ResumeSupport=0\r\n"
    "ResumeThreshold=204800\r\n"
    "\r\n"
    "[Configuration\\Interface\\Editor]\r\n"
    "WordWrap=1\r\n"
    "TabSize=4\r\n"
    "\r\n"
    "[Configuration\\Interface\\Editor\\0]\r\n"
    "FileMask=*.txt;*.log\r\n"
    "Editor=1\r\n"
    "ExternalEditor=gedit%20!.!\r\n"
    "ExternalEditorText=1\r\n"
    "\r\n"
    "[Configuration\\Interface\\Editor\\1]\r\n"
    "FileMask=*.html\r\n"
    "Editor=2\r\n"
    "\r\n"
    "[Configuration\\Interface\\Editor\\2]\r\n"
    "FileMask=*.*\r\n"
    "Editor=0\r\n"
    "\r\n"
    "[Configuration\\Interface\\Commander]\r\n"
    "SwappedPanels=1\r\n"
    "ExplorerStyleSelection=1\r\n"
    "\r\n"
    "[Configuration\\Interface\\QueueView]\r\n"
    "Show=0\r\n"
    "\r\n"
    "[Configuration\\Logging]\r\n"
    "Logging=1\r\n"
    "LogFileName=/tmp/!S.log\r\n"
    "LogProtocol=2\r\n"
    "LogMaxSize=10485760\r\n"
    "LogFileAppend=0\r\n");

  Configuration & C = Configuration::Instance();
  C.SetIniFileName(IniPath);
  C.Load();
  CHECK(C.DoubleClickAction == dcaCopy);
  CHECK(C.CopyOnDoubleClick);
  CHECK(C.QueueTransfersLimit == 5);
  CHECK(!C.DefaultCopyParam().ConfirmOverwriting);
  CHECK(C.DefaultCopyParam().ConfirmResume);
  CHECK(C.DefaultCopyParam().ResumeSupport == CopyParam::rsOn);
  CHECK(C.DefaultCopyParam().ResumeThreshold == 204800);
  CHECK(C.ShowHiddenFiles);
  CHECK(C.FormatSizeBytes == fbShort);
  CHECK(C.PanelSearch == isName);
  CHECK(C.BeepOnFinishAfter == 60);
  CHECK(C.RefreshRemotePanelInterval == 120);
  CHECK(C.DDTransferConfirmation == asOff);
  CHECK(!C.SessionRememberPassword);
  CHECK(C.SessionReopenAuto == 0);
  CHECK(C.PathInCaption == picFull);
  CHECK(!C.ShowLoginOnStartup);
  CHECK(C.EditorWordWrap);
  CHECK(C.EditorTabSize == 4);
  CHECK(C.Editors.size() == 3);
  CHECK(C.SwappedPanels);
  CHECK(C.NortonLikeMode == nlOff);
  CHECK(C.QueueViewShow == qvShow);
  CHECK(C.Logging);
  CHECK(C.LogFileName == QStringLiteral("/tmp/!S.log"));
  CHECK(C.LogProtocol == 2);
  CHECK(C.LogMaxSize == 10485760);
  CHECK(!C.LogFileAppend);
  // Defaults for keys not present
  CHECK(C.ConfirmDeleting);
  CHECK(C.DefaultDirIsHome);
  CHECK(C.SessionReopenAutoStall == 60000);

  // Editors
  CHECK(C.EditorFor(QStringLiteral("readme.txt")) == QStringLiteral("gedit !.!"));
  CHECK(C.EditorFor(QStringLiteral("/var/log/syslog.log")) == QStringLiteral("gedit !.!"));
  CHECK(C.EditorFor(QStringLiteral("index.html")) == QStringLiteral("xdg-open !.!"));
  CHECK(C.EditorFor(QStringLiteral("main.c")).isEmpty());
  CHECK(C.EditorPreferencesFor(QStringLiteral("a.txt"))->ExternalEditorText);
  CHECK(Configuration::FormatEditorCommand(QStringLiteral("gedit !.!"), QStringLiteral("/tmp/a b.txt")) == QStringLiteral("gedit \"/tmp/a b.txt\""));
  CHECK(Configuration::FormatEditorCommand(QStringLiteral("kate"), QStringLiteral("/tmp/x")) == QStringLiteral("kate \"/tmp/x\""));
  CHECK(Configuration::FormatEditorCommand(QStringLiteral("code \"!.!\" -w"), QStringLiteral("/tmp/x")) == QStringLiteral("code \"/tmp/x\" -w"));

  // Modify, save, reload
  C.CopyOnDoubleClick = false;     // legacy flag set by GUI code
  C.QueueTransfersLimit = 3;
  C.EditorCheckNotModified = true;
  C.Editors.removeAt(1);
  C.LogMaxCount = 7;
  C.DarkTheme = asOn;
  C.RefreshRemotePanelInterval = 30;
  C.Save();
  CHECK(C.DoubleClickAction == dcaOpen);

  IniFile Saved;
  CHECK(Saved.Load(IniPath));
  CHECK(Saved.ReadRaw(QStringLiteral("Configuration\\Interface"), QStringLiteral("CopyOnDoubleClick")) == QStringLiteral("0"));
  CHECK(Saved.ReadRaw(QStringLiteral("Configuration\\Interface"), QStringLiteral("QueueTransfersLimit")) == QStringLiteral("3"));
  CHECK(Saved.ReadRaw(QStringLiteral("Configuration\\Interface\\Editor\\1"), QStringLiteral("FileMask")) == QStringLiteral("*.*"));
  CHECK(!Saved.SectionExists(QStringLiteral("Configuration\\Interface\\Editor\\2")));
  CHECK(Saved.ReadRaw(QStringLiteral("Configuration\\Interface\\Editor"), QStringLiteral("TabSize")) == QStringLiteral("4"));
  CHECK(Saved.ReadRaw(QStringLiteral("Configuration\\Logging"), QStringLiteral("LogMaxCount")) == QStringLiteral("7"));

  C.QueueTransfersLimit = 99;
  C.Load();
  CHECK(C.QueueTransfersLimit == 3);
  CHECK(C.EditorCheckNotModified);
  CHECK(C.Editors.size() == 2);
  CHECK(C.EditorFor(QStringLiteral("index.html")).isEmpty());
  CHECK(C.LogMaxCount == 7);
  CHECK(C.DarkTheme == asOn);
  CHECK(C.RefreshRemotePanelInterval == 30);
  CHECK(C.BeepOnFinishAfter == 60);
  CHECK(C.DefaultCopyParam().ResumeSupport == CopyParam::rsOn);
  CHECK(!C.DefaultCopyParam().ConfirmOverwriting);
}

int main()
{
  // Keep Configuration::Instance() away from the user's real ~/.config
  QStandardPaths::setTestModeEnabled(true);
  TestSessionSettings();
  TestAlgoList();
  TestConfiguration();
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
