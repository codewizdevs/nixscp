// Port of WinSCP source/core/SessionData.cpp
#include "core/SessionData.h"
#include "core/IniStorage.h"
#include "core/Security.h"

#include <QCollator>
#include <QDir>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

#include <algorithm>
#include <cstring>

static const QString SessionsKey = QStringLiteral("Sessions");
static const QString DefaultName = QStringLiteral("Default Settings");

bool SessionData::DisablePasswordStoring = false;

SessionData::SessionData()
{
  Default();
}

const char * const SessionData::SshBugNames[sbCount] =
  { "HMAC2", "DeriveKey2", "RSAPad2", "PKSessID2", "Rekey2", "MaxPkt2", "Ignore2", "OldGex2", "WinAdj", "ChanReq" };

const QStringList & SessionData::CipherNames()
{
  static const QStringList Names = {
    QStringLiteral("WARN"), QStringLiteral("3des"), QStringLiteral("blowfish"), QStringLiteral("aes"),
    QStringLiteral("des"), QStringLiteral("arcfour"), QStringLiteral("chacha20"), QStringLiteral("aesgcm") };
  return Names;
}

const QStringList & SessionData::KexNames()
{
  static const QStringList Names = {
    QStringLiteral("WARN"), QStringLiteral("dh-group1-sha1"), QStringLiteral("dh-group14-sha1"),
    QStringLiteral("dh-group15-sha512"), QStringLiteral("dh-group16-sha512"), QStringLiteral("dh-group17-sha512"),
    QStringLiteral("dh-group18-sha512"), QStringLiteral("dh-gex-sha1"), QStringLiteral("rsa"), QStringLiteral("ecdh"),
    QStringLiteral("ntru-curve25519"), QStringLiteral("mlkem-curve25519"), QStringLiteral("mlkem-nist") };
  return Names;
}

const QStringList & SessionData::HostKeyNames()
{
  static const QStringList Names = {
    QStringLiteral("WARN"), QStringLiteral("rsa"), QStringLiteral("dsa"), QStringLiteral("ecdsa"),
    QStringLiteral("ed25519"), QStringLiteral("ed448") };
  return Names;
}

// DefaultCipherList, DefaultKexList, DefaultHostKeyList
QString SessionData::DefaultCipherList()
{
  return QStringLiteral("aes,chacha20,aesgcm,3des,WARN,des,blowfish,arcfour");
}

QString SessionData::DefaultKexList()
{
  return QStringLiteral("ntru-curve25519,mlkem-curve25519,mlkem-nist,ecdh,dh-gex-sha1,dh-group18-sha512,"
    "dh-group17-sha512,dh-group16-sha512,dh-group15-sha512,dh-group14-sha1,rsa,WARN,dh-group1-sha1");
}

QString SessionData::DefaultHostKeyList()
{
  return QStringLiteral("ed448,ed25519,ecdsa,rsa,dsa,WARN");
}

QStringList SessionData::NormalizeAlgoList(const QString & List, const QString & Defaults)
{
  // Simplified TSessionData::SetAlgoList: algorithms missing in the custom
  // list are inserted at their default position when they precede the first
  // listed algorithm, otherwise above "WARN" (or at the end)
  QStringList DefaultList = Defaults.split(QLatin1Char(','));
  QStringList Result;
  for (const QString & Name : List.split(QLatin1Char(','), Qt::SkipEmptyParts))
  {
    QString N = Name.trimmed();
    if (DefaultList.contains(N) && !Result.contains(N))
    {
      Result.append(N);
    }
  }
  bool Priority = true;
  for (int Index = 0; Index < DefaultList.size(); Index++)
  {
    const QString & Algo = DefaultList[Index];
    if (!Result.contains(Algo))
    {
      if (Priority)
      {
        Result.insert(qMin(Index, static_cast<int>(Result.size())), Algo);
      }
      else
      {
        int WarnIndex = Result.indexOf(QStringLiteral("WARN"));
        int WarnDefaultIndex = DefaultList.indexOf(QStringLiteral("WARN"));
        if ((WarnIndex >= 0) && (Index < WarnDefaultIndex))
        {
          Result.insert(WarnIndex, Algo);
        }
        else
        {
          Result.append(Algo);
        }
      }
    }
    else
    {
      Priority = false;
    }
  }
  return Result;
}

qint64 SessionData::RekeyBytes() const
{
  // PuTTY parse_blocksize
  QString S = RekeyData.trimmed();
  qint64 Multiplier = 1;
  if (!S.isEmpty())
  {
    QChar Suffix = S.back().toUpper();
    if (Suffix == QLatin1Char('K')) Multiplier = 1024;
    else if (Suffix == QLatin1Char('M')) Multiplier = 1024 * 1024;
    else if (Suffix == QLatin1Char('G')) Multiplier = 1024 * 1024 * 1024;
    if (Multiplier > 1)
    {
      S.chop(1);
    }
  }
  return S.toLongLong() * Multiplier;
}

void SessionData::Default()
{
  // TSessionData::DefaultSettings
  HostName.clear();
  PortNumber = SshPortNumber;
  UserName.clear();
  Password.clear();
  FSProtocol = fsSFTP;
  Ftps = ftpsNone;
  IsWorkspace = false;
  NoteOnly = false;
  Link.clear();
  NameOverride.clear();
  Color = 0;
  Note.clear();
  AgentAccess = 0;
  AgentPermissions = 0x07;
  AgentName.clear();
  AgentAllow.clear();
  AgentDeny.clear();
  AgentRecord = true;
  PortForwards.clear();
  Special = false;

  EOLType = eolLF;
  NotUtf = asAuto;
  DSTMode = dstmUnix;
  TimeDifference = 0;
  TimeDifferenceAuto = true;
  TrimVMSVersions = false;
  VMSAllRevisions = false;

  RemoteDirectory.clear();
  LocalDirectory.clear();
  OtherLocalDirectory.clear();
  SynchronizeBrowsing = false;
  UpdateDirectories = true;
  CacheDirectories = true;
  CacheDirectoryChanges = true;
  PreserveDirectoryChanges = true;
  ResolveSymlinks = true;
  FollowDirectorySymlinks = false;

  DeleteToRecycleBin = false;
  OverwrittenToRecycleBin = false;
  RecycleBinPath.clear();

  SftpServer.clear();
  SFTPMaxVersion = SFTPMaxVersionAuto;
  SFTPMaxPacketSize = 0;
  SFTPDownloadQueue = 32;
  SFTPUploadQueue = 64;
  SFTPListingQueue = 2;
  SFTPRealPath = asAuto;
  UsePosixRename = false;
  for (TAutoSwitch & B : SFTPBug)
  {
    B = asAuto;
  }

  Shell.clear();
  ReturnVar.clear();
  ExitCode1IsError = false;
  LookupUserGroups = asAuto;
  ClearAliases = true;
  UnsetNationalVars = true;
  ListingCommand = QStringLiteral("ls -la");
  IgnoreLsWarnings = true;
  SCPLsFullTime = asAuto;
  Scp1Compatibility = false;
  PostLoginCommands.clear();

  FtpPasvMode = true;
  FtpForcePasvIp = asAuto;
  FtpUseMlsd = asAuto;
  FtpAccount.clear();
  FtpPingType = fptDummyCommand;
  FtpPingInterval = 30;
  FtpTransferActiveImmediately = asAuto;
  FtpListAll = asAuto;
  FtpHost = asAuto;
  FtpWorkFromCwd = asAuto;
  FtpAnyCodeForPwd = false;
  FtpProxyLogonType = 0;
  SslSessionReuse = true;
  TlsCertificateFile.clear();
  MinTlsVersion = tlsDefaultMin;
  MaxTlsVersion = tlsMax;
  CompleteTlsShutdown = asAuto;
  ProtocolFeatures.clear();

  S3DefaultRegion.clear();
  S3SessionToken.clear();
  S3RoleArn.clear();
  S3RoleSessionName.clear();
  S3Profile.clear();
  S3UrlStyle = s3usVirtualHost;
  S3MaxKeys = asAuto;
  S3CredentialsEnv = false;
  S3RequesterPays = false;
  WebDavLiberalEscaping = false;
  WebDavAuthLegacy = false;

  Timeout = 15;
  PingType = ptOff;
  PingInterval = 30;
  AddressFamily = afAuto;
  TcpNoDelay = false;
  SendBuf = DefaultSendBuf;
  SourceAddress.clear();

  // DefaultProxy
  ProxyMethod = pmNone;
  ProxyHost = QStringLiteral("proxy");
  ProxyPort = ProxyPortNumber;
  ProxyUsername.clear();
  ProxyPassword.clear();
  ProxyTelnetCommand = QStringLiteral("connect %host %port\\n");
  ProxyLocalCommand.clear();
  ProxyDNS = asAuto;
  ProxyLocalhost = false;

  Tunnel = false;
  TunnelHostName.clear();
  TunnelPortNumber = SshPortNumber;
  TunnelUserName.clear();
  TunnelPassword.clear();
  TunnelPublicKeyFile.clear();
  TunnelPassphrase.clear();
  TunnelLocalPortNumber = 0;
  TunnelHostKey.clear();

  Compression = false;
  Ssh2DES = false;
  CipherList = DefaultCipherList();
  KexList = DefaultKexList();
  HostKeyList = DefaultHostKeyList();
  RekeyData = QStringLiteral("1G");
  RekeyTime = 60;
  HostKey.clear();
  SshSimple = true;

  SshNoUserAuth = false;
  TryAgent = true;
  AuthKI = true;
  AuthKIPassword = true;
  AgentFwd = false;
  AuthGSSAPI = true;
  AuthGSSAPIKEX = false;
  GSSAPIFwdTGT = false;
  ChangeUsername = false;
  LogicalHostName.clear();
  PublicKeyFile.clear();
  DetachedCertificate.clear();

  for (TAutoSwitch & B : Bug)
  {
    B = asAuto;
  }

  KnownHostsFile.clear();
  RequireDirectories = false;
  HostKeyFromUrl = false;
  TunnelHostKeyFromUrl = false;
  NoExpansion = false;
  FRawValues.clear();
}

// THierarchicalStorage::ReadEnum with a name mapping (TIntMapping):
// the value may be stored as a (case insensitive) name or as an integer
static int ReadEnum(IniStorage & Storage, const QString & Name, int Default, const QStringList & Names, const QList<int> & Values)
{
  QString S = Storage.ReadStringRaw(Name, QString()).trimmed().toLower();
  int Index = Names.indexOf(S);
  if (!S.isEmpty() && (Index >= 0))
  {
    return Values[Index];
  }
  return Storage.ReadInteger(Name, Default);
}

static TAutoSwitch ReadAutoSwitch(IniStorage & Storage, const QString & Name, TAutoSwitch Default, bool Reversed = false)
{
  // AutoSwitchMapping / AutoSwitchReversedMapping
  return static_cast<TAutoSwitch>(ReadEnum(Storage, Name, Default,
    {QStringLiteral("on"), QStringLiteral("off"), QStringLiteral("auto")},
    {Reversed ? asOff : asOn, Reversed ? asOn : asOff, asAuto}));
}

// THierarchicalStorage::ReadFloat/WriteFloat: the double is stored as hex of its bytes
static double ReadFloat(IniStorage & Storage, const QString & Name, double Default)
{
  QString Value = Storage.ReadStringRaw(Name, QString());
  if (Value.isEmpty())
  {
    return Default;
  }
  QByteArray Raw = QByteArray::fromHex(Value.toLatin1());
  double Result;
  if ((Raw.size() == static_cast<int>(sizeof(Result))) && (Raw.toHex().toUpper() == Value.toLatin1().toUpper()))
  {
    memcpy(&Result, Raw.constData(), sizeof(Result));
    return Result;
  }
  bool Ok;
  Result = Value.toDouble(&Ok);
  return Ok ? Result : Default;
}

static QString FloatToHex(double Value)
{
  return QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(&Value), sizeof(Value)).toHex().toUpper());
}

void SessionData::Load(IniStorage & Storage)
{
  // TSessionData::DoLoad
  FRawValues.clear();
  for (const QString & Key : Storage.GetValueNames())
  {
    FRawValues.insert(Key, Storage.ReadStringRaw(Key, QString()));
  }

  PortNumber = Storage.ReadInteger(QStringLiteral("PortNumber"), PortNumber);
  UserName = Storage.ReadString(QStringLiteral("UserName"), UserName);
  // must be loaded after UserName, because HostName may be in format user@host
  HostName = Storage.ReadString(QStringLiteral("HostName"), HostName);
  int At = HostName.lastIndexOf(QLatin1Char('@'));
  if (At >= 0)
  {
    UserName = HostName.left(At);
    HostName = HostName.mid(At + 1);
  }

  bool LoadPasswords = !DisablePasswordStoring;
  if (!LoadPasswords)
  {
    // not loaded (nor kept for saving)
  }
  else if (Storage.ValueExists(QStringLiteral("PasswordPlain")))
  {
    Password = Storage.ReadString(QStringLiteral("PasswordPlain"), Password);
  }
  else if (Storage.ValueExists(QStringLiteral("Password")))
  {
    Password = Security::DecryptPassword(Storage.ReadStringRaw(QStringLiteral("Password"), QString()).toLatin1(), UserName + HostName);
  }
  HostKey = Storage.ReadString(QStringLiteral("SshHostKey"), HostKey);
  Note = Storage.ReadString(QStringLiteral("Note"), Note);
  AgentAccess = Storage.ReadInteger(QStringLiteral("AgentAccess"), AgentAccess);
  AgentPermissions = Storage.ReadInteger(QStringLiteral("AgentPermissions"), AgentPermissions);
  AgentName = Storage.ReadString(QStringLiteral("AgentName"), AgentName);
  AgentAllow = Storage.ReadString(QStringLiteral("AgentAllow"), AgentAllow);
  AgentDeny = Storage.ReadString(QStringLiteral("AgentDeny"), AgentDeny);
  AgentRecord = Storage.ReadBool(QStringLiteral("AgentRecord"), AgentRecord);
  PortForwards = Storage.ReadString(QStringLiteral("PortForwards"), PortForwards);
  // Putty uses PingIntervalSecs
  int PingIntervalSecs = Storage.ReadInteger(QStringLiteral("PingIntervalSecs"), -1);
  if (PingIntervalSecs < 0)
  {
    PingIntervalSecs = Storage.ReadInteger(QStringLiteral("PingIntervalSec"), PingInterval % 60);
  }
  PingInterval = Storage.ReadInteger(QStringLiteral("PingInterval"), PingInterval / 60) * 60 + PingIntervalSecs;
  if (PingInterval == 0)
  {
    PingInterval = 30;
  }
  PingType = static_cast<TPingType>(Storage.ReadInteger(QStringLiteral("PingType"), PingType));
  Timeout = Storage.ReadInteger(QStringLiteral("Timeout"), Timeout);
  TryAgent = Storage.ReadBool(QStringLiteral("TryAgent"), TryAgent);
  AgentFwd = Storage.ReadBool(QStringLiteral("AgentFwd"), AgentFwd);
  AuthKI = Storage.ReadBool(QStringLiteral("AuthKI"), AuthKI);
  AuthKIPassword = Storage.ReadBool(QStringLiteral("AuthKIPassword"), AuthKIPassword);
  AuthGSSAPI = Storage.ReadBool(QStringLiteral("AuthGSSAPI"), Storage.ReadBool(QStringLiteral("AuthSSPI"), AuthGSSAPI));
  AuthGSSAPIKEX = Storage.ReadBool(QStringLiteral("AuthGSSAPIKEX"), AuthGSSAPIKEX);
  GSSAPIFwdTGT = Storage.ReadBool(QStringLiteral("GSSAPIFwdTGT"),
    Storage.ReadBool(QStringLiteral("GssapiFwd"), Storage.ReadBool(QStringLiteral("SSPIFwdTGT"), GSSAPIFwdTGT)));
  LogicalHostName = Storage.ReadString(QStringLiteral("LogicalHostName"),
    Storage.ReadString(QStringLiteral("GSSAPIServerRealm"), Storage.ReadString(QStringLiteral("KerbPrincipal"), LogicalHostName)));
  ChangeUsername = Storage.ReadBool(QStringLiteral("ChangeUsername"), ChangeUsername);
  Compression = Storage.ReadBool(QStringLiteral("Compression"), Compression);
  Ssh2DES = Storage.ReadBool(QStringLiteral("Ssh2DES"), Ssh2DES);
  SshNoUserAuth = Storage.ReadBool(QStringLiteral("SshNoUserAuth"), SshNoUserAuth);
  CipherList = Storage.ReadString(QStringLiteral("Cipher"), CipherList);
  KexList = Storage.ReadString(QStringLiteral("KEX"), KexList);
  HostKeyList = Storage.ReadString(QStringLiteral("HostKey"), HostKeyList);
  PublicKeyFile = Storage.ReadString(QStringLiteral("PublicKeyFile"), PublicKeyFile);
  DetachedCertificate = Storage.ReadString(QStringLiteral("DetachedCertificate"), DetachedCertificate);
  AddressFamily = static_cast<TAddressFamily>(Storage.ReadInteger(QStringLiteral("AddressFamily"), AddressFamily));
  RekeyData = Storage.ReadString(QStringLiteral("RekeyBytes"), RekeyData);
  RekeyTime = Storage.ReadInteger(QStringLiteral("RekeyTime"), RekeyTime);

  FSProtocol = static_cast<TFSProtocol>(Storage.ReadInteger(QStringLiteral("FSProtocol"), FSProtocol));
  LocalDirectory = Storage.ReadString(QStringLiteral("LocalDirectory"), LocalDirectory);
  OtherLocalDirectory = Storage.ReadString(QStringLiteral("OtherLocalDirectory"), OtherLocalDirectory);
  RemoteDirectory = Storage.ReadString(QStringLiteral("RemoteDirectory"), RemoteDirectory);
  SynchronizeBrowsing = Storage.ReadBool(QStringLiteral("SynchronizeBrowsing"), SynchronizeBrowsing);
  UpdateDirectories = Storage.ReadBool(QStringLiteral("UpdateDirectories"), UpdateDirectories);
  CacheDirectories = Storage.ReadBool(QStringLiteral("CacheDirectories"), CacheDirectories);
  CacheDirectoryChanges = Storage.ReadBool(QStringLiteral("CacheDirectoryChanges"), CacheDirectoryChanges);
  PreserveDirectoryChanges = Storage.ReadBool(QStringLiteral("PreserveDirectoryChanges"), PreserveDirectoryChanges);
  ResolveSymlinks = Storage.ReadBool(QStringLiteral("ResolveSymlinks"), ResolveSymlinks);
  FollowDirectorySymlinks = Storage.ReadBool(QStringLiteral("FollowDirectorySymlinks"), FollowDirectorySymlinks);
  DSTMode = static_cast<TDSTMode>(Storage.ReadInteger(QStringLiteral("ConsiderDST"), DSTMode));
  Shell = Storage.ReadString(QStringLiteral("Shell"), Shell);
  ClearAliases = Storage.ReadBool(QStringLiteral("ClearAliases"), ClearAliases);
  UnsetNationalVars = Storage.ReadBool(QStringLiteral("UnsetNationalVars"), UnsetNationalVars);
  ListingCommand = Storage.ReadString(QStringLiteral("ListingCommand"),
    Storage.ReadBool(QStringLiteral("AliasGroupList"), false) ? QStringLiteral("ls -gla") : ListingCommand);
  IgnoreLsWarnings = Storage.ReadBool(QStringLiteral("IgnoreLsWarnings"), IgnoreLsWarnings);
  SCPLsFullTime = ReadAutoSwitch(Storage, QStringLiteral("SCPLsFullTime"), SCPLsFullTime);
  Scp1Compatibility = Storage.ReadBool(QStringLiteral("Scp1Compatibility"), Scp1Compatibility);
  TimeDifference = ReadFloat(Storage, QStringLiteral("TimeDifference"), TimeDifference);
  TimeDifferenceAuto = Storage.ReadBool(QStringLiteral("TimeDifferenceAuto"), (TimeDifference == 0));
  DeleteToRecycleBin = Storage.ReadBool(QStringLiteral("DeleteToRecycleBin"), DeleteToRecycleBin);
  OverwrittenToRecycleBin = Storage.ReadBool(QStringLiteral("OverwrittenToRecycleBin"), OverwrittenToRecycleBin);
  RecycleBinPath = Storage.ReadString(QStringLiteral("RecycleBinPath"), RecycleBinPath);
  PostLoginCommands = Storage.ReadString(QStringLiteral("PostLoginCommands"), PostLoginCommands);
  ReturnVar = Storage.ReadString(QStringLiteral("ReturnVar"), ReturnVar);
  ExitCode1IsError = Storage.ReadBool(QStringLiteral("ExitCode1IsError"), ExitCode1IsError);
  LookupUserGroups = ReadAutoSwitch(Storage, QStringLiteral("LookupUserGroups2"), LookupUserGroups);
  EOLType = static_cast<TEOLType>(Storage.ReadInteger(QStringLiteral("EOLType"), EOLType));
  TrimVMSVersions = Storage.ReadBool(QStringLiteral("TrimVMSVersions"), TrimVMSVersions);
  VMSAllRevisions = Storage.ReadBool(QStringLiteral("VMSAllRevisions"), VMSAllRevisions);
  NotUtf = ReadAutoSwitch(Storage, QStringLiteral("Utf"), ReadAutoSwitch(Storage, QStringLiteral("SFTPUtfBug"), NotUtf, true), true);

  S3DefaultRegion = Storage.ReadString(QStringLiteral("S3DefaultRegion"), S3DefaultRegion);
  S3SessionToken = Storage.ReadString(QStringLiteral("S3SessionToken"), S3SessionToken);
  S3RoleArn = Storage.ReadString(QStringLiteral("S3RoleArn"), S3RoleArn);
  S3RoleSessionName = Storage.ReadString(QStringLiteral("S3RoleSessionName"), S3RoleSessionName);
  S3Profile = Storage.ReadString(QStringLiteral("S3Profile"), S3Profile);
  S3UrlStyle = static_cast<TS3UrlStyle>(Storage.ReadInteger(QStringLiteral("S3UrlStyle"), S3UrlStyle));
  S3MaxKeys = ReadAutoSwitch(Storage, QStringLiteral("S3MaxKeys"), S3MaxKeys);
  S3CredentialsEnv = Storage.ReadBool(QStringLiteral("S3CredentialsEnv"), S3CredentialsEnv);
  S3RequesterPays = Storage.ReadBool(QStringLiteral("S3RequesterPays"), S3RequesterPays);

  TcpNoDelay = Storage.ReadBool(QStringLiteral("TcpNoDelay"), TcpNoDelay);
  SendBuf = Storage.ReadInteger(QStringLiteral("SendBuf"), Storage.ReadInteger(QStringLiteral("SshSendBuf"), SendBuf));
  SourceAddress = Storage.ReadString(QStringLiteral("SourceAddress"), SourceAddress);
  SshSimple = Storage.ReadBool(QStringLiteral("SshSimple"), SshSimple);

  // ProxyMethodMapping: lower-cased ProxyMethodNames
  ProxyMethod = static_cast<TProxyMethod>(ReadEnum(Storage, QStringLiteral("ProxyMethod"), ProxyMethod,
    {QStringLiteral("none"), QStringLiteral("socks4"), QStringLiteral("socks5"), QStringLiteral("http"), QStringLiteral("telnet"), QStringLiteral("cmd")},
    {pmNone, pmSocks4, pmSocks5, pmHTTP, pmTelnet, pmCmd}));
  ProxyHost = Storage.ReadString(QStringLiteral("ProxyHost"), ProxyHost);
  ProxyPort = Storage.ReadInteger(QStringLiteral("ProxyPort"), ProxyPort);
  ProxyUsername = Storage.ReadString(QStringLiteral("ProxyUsername"), ProxyUsername);
  if (Storage.ValueExists(QStringLiteral("ProxyPassword")))
  {
    ProxyPassword = Storage.ReadString(QStringLiteral("ProxyPassword"), ProxyPassword);
  }
  else if (Storage.ValueExists(QStringLiteral("ProxyPasswordEnc")))
  {
    ProxyPassword = Security::DecryptPassword(Storage.ReadStringRaw(QStringLiteral("ProxyPasswordEnc"), QString()).toLatin1(), ProxyUsername + ProxyHost);
  }
  if (ProxyMethod == pmCmd)
  {
    ProxyLocalCommand = Storage.ReadStringRaw(QStringLiteral("ProxyTelnetCommand"), ProxyLocalCommand);
  }
  else
  {
    ProxyTelnetCommand = Storage.ReadStringRaw(QStringLiteral("ProxyTelnetCommand"), ProxyTelnetCommand);
  }
  ProxyDNS = static_cast<TAutoSwitch>((Storage.ReadInteger(QStringLiteral("ProxyDNS"), (ProxyDNS + 2) % 3) + 1) % 3);
  ProxyLocalhost = Storage.ReadBool(QStringLiteral("ProxyLocalhost"), ProxyLocalhost);

  for (int Index = 0; Index < sbCount; Index++)
  {
    QString Key = QStringLiteral("Bug") + QLatin1String(SshBugNames[Index]);
    Bug[Index] = static_cast<TAutoSwitch>(2 - Storage.ReadInteger(Key, 2 - Bug[Index]));
  }
  if ((Bug[sbHMAC2] == asAuto) && Storage.ReadBool(QStringLiteral("BuggyMAC"), false))
  {
    Bug[sbHMAC2] = asOn;
  }

  SftpServer = Storage.ReadString(QStringLiteral("SftpServer"), SftpServer);
  SFTPBug[sbSymlink] = ReadAutoSwitch(Storage, QStringLiteral("SFTPSymlinkBug"), SFTPBug[sbSymlink]);
  SFTPBug[sbSignedTS] = ReadAutoSwitch(Storage, QStringLiteral("SFTPSignedTSBug"), SFTPBug[sbSignedTS]);
  SFTPMaxVersion = Storage.ReadInteger(QStringLiteral("SFTPMaxVersion"), SFTPMaxVersion);
  SFTPMaxPacketSize = Storage.ReadInteger(QStringLiteral("SFTPMaxPacketSize"), SFTPMaxPacketSize);
  SFTPDownloadQueue = Storage.ReadInteger(QStringLiteral("SFTPDownloadQueue"), SFTPDownloadQueue);
  SFTPUploadQueue = Storage.ReadInteger(QStringLiteral("SFTPUploadQueue"), SFTPUploadQueue);
  SFTPListingQueue = Storage.ReadInteger(QStringLiteral("SFTPListingQueue"), SFTPListingQueue);
  SFTPRealPath = ReadAutoSwitch(Storage, QStringLiteral("SFTPRealPath"), SFTPRealPath);
  UsePosixRename = Storage.ReadBool(QStringLiteral("UsePosixRename"), UsePosixRename);

  Color = Storage.ReadInteger(QStringLiteral("Color"), Color);

  Tunnel = Storage.ReadBool(QStringLiteral("Tunnel"), Tunnel);
  TunnelPortNumber = Storage.ReadInteger(QStringLiteral("TunnelPortNumber"), TunnelPortNumber);
  TunnelUserName = Storage.ReadString(QStringLiteral("TunnelUserName"), TunnelUserName);
  // must be loaded after TunnelUserName, because TunnelHostName may be in format user@host
  TunnelHostName = Storage.ReadString(QStringLiteral("TunnelHostName"), TunnelHostName);
  At = TunnelHostName.lastIndexOf(QLatin1Char('@'));
  if (At >= 0)
  {
    TunnelUserName = TunnelHostName.left(At);
    TunnelHostName = TunnelHostName.mid(At + 1);
  }
  if (!LoadPasswords)
  {
    // see Password
  }
  else if (Storage.ValueExists(QStringLiteral("TunnelPasswordPlain")))
  {
    TunnelPassword = Storage.ReadString(QStringLiteral("TunnelPasswordPlain"), TunnelPassword);
  }
  else if (Storage.ValueExists(QStringLiteral("TunnelPassword")))
  {
    TunnelPassword = Security::DecryptPassword(Storage.ReadStringRaw(QStringLiteral("TunnelPassword"), QString()).toLatin1(), TunnelUserName + TunnelHostName);
  }
  TunnelPublicKeyFile = Storage.ReadString(QStringLiteral("TunnelPublicKeyFile"), TunnelPublicKeyFile);
  // Contrary to main session passphrase (which has -passphrase switch in scripting),
  // the tunnel passphrase is loaded, as there's no other way to provide it in scripting
  if (!LoadPasswords)
  {
    // see Password
  }
  else if (Storage.ValueExists(QStringLiteral("TunnelPassphrasePlain")))
  {
    TunnelPassphrase = Storage.ReadString(QStringLiteral("TunnelPassphrasePlain"), TunnelPassphrase);
  }
  else if (Storage.ValueExists(QStringLiteral("TunnelPassphrase")))
  {
    TunnelPassphrase = Security::DecryptPassword(Storage.ReadStringRaw(QStringLiteral("TunnelPassphrase"), QString()).toLatin1(), TunnelPublicKeyFile);
  }
  TunnelLocalPortNumber = Storage.ReadInteger(QStringLiteral("TunnelLocalPortNumber"), TunnelLocalPortNumber);
  TunnelHostKey = Storage.ReadString(QStringLiteral("TunnelHostKey"), TunnelHostKey);

  FtpPasvMode = Storage.ReadBool(QStringLiteral("FtpPasvMode"), FtpPasvMode);
  FtpForcePasvIp = ReadAutoSwitch(Storage, QStringLiteral("FtpForcePasvIp2"), FtpForcePasvIp);
  FtpUseMlsd = ReadAutoSwitch(Storage, QStringLiteral("FtpUseMlsd"), FtpUseMlsd);
  FtpAccount = Storage.ReadString(QStringLiteral("FtpAccount"), FtpAccount);
  FtpPingInterval = Storage.ReadInteger(QStringLiteral("FtpPingInterval"), FtpPingInterval);
  FtpPingType = static_cast<TFtpPingType>(Storage.ReadInteger(QStringLiteral("FtpPingType"), FtpPingType));
  FtpTransferActiveImmediately = ReadAutoSwitch(Storage, QStringLiteral("FtpTransferActiveImmediately2"), FtpTransferActiveImmediately);
  Ftps = static_cast<TFtps>(Storage.ReadInteger(QStringLiteral("Ftps"), Ftps));
  FtpListAll = ReadAutoSwitch(Storage, QStringLiteral("FtpListAll"), FtpListAll);
  FtpHost = ReadAutoSwitch(Storage, QStringLiteral("FtpHost"), FtpHost);
  FtpWorkFromCwd = ReadAutoSwitch(Storage, QStringLiteral("FtpWorkFromCwd"), ReadAutoSwitch(Storage, QStringLiteral("FtpDeleteFromCwd"), FtpWorkFromCwd));
  FtpAnyCodeForPwd = Storage.ReadBool(QStringLiteral("FtpAnyCodeForPwd"), FtpAnyCodeForPwd);
  SslSessionReuse = Storage.ReadBool(QStringLiteral("SslSessionReuse"), SslSessionReuse);
  TlsCertificateFile = Storage.ReadString(QStringLiteral("TlsCertificateFile"), TlsCertificateFile);
  FtpProxyLogonType = Storage.ReadInteger(QStringLiteral("FtpProxyLogonType"), FtpProxyLogonType);
  MinTlsVersion = static_cast<TTlsVersion>(Storage.ReadInteger(QStringLiteral("MinTlsVersion"), MinTlsVersion));
  MaxTlsVersion = static_cast<TTlsVersion>(Storage.ReadInteger(QStringLiteral("MaxTlsVersion"), MaxTlsVersion));
  CompleteTlsShutdown = ReadAutoSwitch(Storage, QStringLiteral("CompleteTlsShutdown"), CompleteTlsShutdown);
  ProtocolFeatures = Storage.ReadString(QStringLiteral("ProtocolFeatures"), ProtocolFeatures);

  NoExpansion = Storage.ReadBool(QStringLiteral("NoExpansion"), NoExpansion);
  WebDavLiberalEscaping = Storage.ReadBool(QStringLiteral("WebDavLiberalEscaping"), WebDavLiberalEscaping);
  WebDavAuthLegacy = Storage.ReadBool(QStringLiteral("WebDavAuthLegacy"), WebDavAuthLegacy);

  IsWorkspace = Storage.ReadBool(QStringLiteral("IsWorkspace"), IsWorkspace);
  NoteOnly = Storage.ReadBool(QStringLiteral("NoteOnly"), NoteOnly);
  Link = Storage.ReadString(QStringLiteral("Link"), Link);
  NameOverride = Storage.ReadString(QStringLiteral("NameOverride"), NameOverride);
  // never written by Save (kept in FRawValues)
  Special = Storage.ReadBool(QStringLiteral("Special"), Special);
}

void SessionData::Save(IniStorage & Storage, const SessionData * Default) const
{
  // Preserve values not (yet) handled by NixSCP
  for (auto It = FRawValues.cbegin(); It != FRawValues.cend(); ++It)
  {
    Storage.WriteStringRaw(It.key(), It.value());
  }

  // TSessionData::DoSave
  #define WRITE_DATA_EX(TYPE, NAME, PROPERTY) \
    if ((Default != nullptr) && (Default->PROPERTY == PROPERTY)) \
    { \
      Storage.DeleteValue(QStringLiteral(NAME)); \
    } \
    else \
    { \
      Storage.Write ## TYPE(QStringLiteral(NAME), PROPERTY); \
    }
  #define WRITE_DATA(TYPE, PROPERTY) WRITE_DATA_EX(TYPE, #PROPERTY, PROPERTY)

  WRITE_DATA(String, HostName);
  WRITE_DATA(Integer, PortNumber);
  if ((Default != nullptr) && (Default->PingInterval == PingInterval))
  {
    Storage.DeleteValue(QStringLiteral("PingInterval"));
    Storage.DeleteValue(QStringLiteral("PingIntervalSecs"));
  }
  else
  {
    Storage.WriteInteger(QStringLiteral("PingInterval"), PingInterval / 60);
    Storage.WriteInteger(QStringLiteral("PingIntervalSecs"), PingInterval % 60);
  }
  Storage.DeleteValue(QStringLiteral("PingIntervalSec")); // obsolete
  WRITE_DATA(Integer, PingType);
  WRITE_DATA(Integer, Timeout);
  WRITE_DATA(Bool, TryAgent);
  WRITE_DATA(Bool, AgentFwd);
  WRITE_DATA(Bool, AuthKI);
  WRITE_DATA(Bool, AuthKIPassword);
  // Saved, what a URL said would be a configured key, which outranks known_hosts
  if (HostKeyFromUrl)
  {
    Storage.DeleteValue(QStringLiteral("SshHostKey"));
  }
  else
  {
    WRITE_DATA_EX(String, "SshHostKey", HostKey);
  }
  WRITE_DATA(String, Note);
  WRITE_DATA(Integer, AgentAccess);
  WRITE_DATA(Integer, AgentPermissions);
  WRITE_DATA(String, AgentName);
  WRITE_DATA(String, AgentAllow);
  WRITE_DATA(String, AgentDeny);
  WRITE_DATA(Bool, AgentRecord);
  WRITE_DATA(String, PortForwards);
  WRITE_DATA(Bool, AuthGSSAPI);
  WRITE_DATA(Bool, AuthGSSAPIKEX);
  WRITE_DATA(Bool, GSSAPIFwdTGT);
  WRITE_DATA(String, LogicalHostName);
  WRITE_DATA(Bool, ChangeUsername);
  WRITE_DATA(Bool, Compression);
  WRITE_DATA(Bool, Ssh2DES);
  WRITE_DATA(Bool, SshNoUserAuth);
  WRITE_DATA_EX(String, "Cipher", CipherList);
  WRITE_DATA_EX(String, "KEX", KexList);
  WRITE_DATA_EX(String, "HostKey", HostKeyList);
  WRITE_DATA(Integer, AddressFamily);
  WRITE_DATA_EX(String, "RekeyBytes", RekeyData);
  WRITE_DATA(Integer, RekeyTime);
  WRITE_DATA(Bool, TcpNoDelay);

  WRITE_DATA(String, UserName);
  WRITE_DATA(String, PublicKeyFile);
  WRITE_DATA(String, DetachedCertificate);
  WRITE_DATA(Integer, FSProtocol);
  WRITE_DATA(String, LocalDirectory);
  WRITE_DATA(String, OtherLocalDirectory);
  WRITE_DATA(String, RemoteDirectory);
  WRITE_DATA(Bool, SynchronizeBrowsing);
  WRITE_DATA(Bool, UpdateDirectories);
  WRITE_DATA(Bool, CacheDirectories);
  WRITE_DATA(Bool, CacheDirectoryChanges);
  WRITE_DATA(Bool, PreserveDirectoryChanges);
  WRITE_DATA(Bool, ResolveSymlinks);
  WRITE_DATA(Bool, FollowDirectorySymlinks);
  WRITE_DATA_EX(Integer, "ConsiderDST", DSTMode);
  WRITE_DATA(String, Shell);
  WRITE_DATA(Bool, ClearAliases);
  WRITE_DATA(Bool, UnsetNationalVars);
  WRITE_DATA(String, ListingCommand);
  WRITE_DATA(Bool, IgnoreLsWarnings);
  WRITE_DATA(Integer, SCPLsFullTime);
  WRITE_DATA(Bool, Scp1Compatibility);
  // TimeDifferenceAuto is valid for FTP protocol only
  if (TimeDifferenceAuto && (FSProtocol == fsFTP))
  {
    Storage.DeleteValue(QStringLiteral("TimeDifference"));
  }
  else if ((Default != nullptr) && (Default->TimeDifference == TimeDifference))
  {
    Storage.DeleteValue(QStringLiteral("TimeDifference"));
  }
  else
  {
    Storage.WriteStringRaw(QStringLiteral("TimeDifference"), FloatToHex(TimeDifference));
  }
  WRITE_DATA(Bool, TimeDifferenceAuto);
  WRITE_DATA(Bool, DeleteToRecycleBin);
  WRITE_DATA(Bool, OverwrittenToRecycleBin);
  WRITE_DATA(String, RecycleBinPath);
  WRITE_DATA(String, PostLoginCommands);
  WRITE_DATA(String, ReturnVar);
  WRITE_DATA(Bool, ExitCode1IsError);
  WRITE_DATA_EX(Integer, "LookupUserGroups2", LookupUserGroups);
  WRITE_DATA(Integer, EOLType);
  WRITE_DATA(Bool, TrimVMSVersions);
  WRITE_DATA(Bool, VMSAllRevisions);
  Storage.DeleteValue(QStringLiteral("SFTPUtfBug"));
  WRITE_DATA_EX(Integer, "Utf", NotUtf);
  WRITE_DATA(String, S3DefaultRegion);
  WRITE_DATA(String, S3SessionToken);
  WRITE_DATA(String, S3RoleArn);
  WRITE_DATA(String, S3RoleSessionName);
  WRITE_DATA(String, S3Profile);
  WRITE_DATA(Integer, S3UrlStyle);
  WRITE_DATA(Integer, S3MaxKeys);
  WRITE_DATA(Bool, S3CredentialsEnv);
  WRITE_DATA(Bool, S3RequesterPays);
  WRITE_DATA(Integer, SendBuf);
  WRITE_DATA(String, SourceAddress);
  WRITE_DATA(Bool, SshSimple);

  WRITE_DATA(Integer, ProxyMethod);
  WRITE_DATA(String, ProxyHost);
  WRITE_DATA(Integer, ProxyPort);
  WRITE_DATA(String, ProxyUsername);
  if (ProxyMethod == pmCmd)
  {
    WRITE_DATA_EX(StringRaw, "ProxyTelnetCommand", ProxyLocalCommand);
  }
  else
  {
    WRITE_DATA(StringRaw, ProxyTelnetCommand);
  }
  if ((Default != nullptr) && (Default->ProxyDNS == ProxyDNS))
  {
    Storage.DeleteValue(QStringLiteral("ProxyDNS"));
  }
  else
  {
    Storage.WriteInteger(QStringLiteral("ProxyDNS"), (ProxyDNS + 2) % 3);
  }
  WRITE_DATA(Bool, ProxyLocalhost);

  for (int Index = 0; Index < sbCount; Index++)
  {
    QString Key = QStringLiteral("Bug") + QLatin1String(SshBugNames[Index]);
    if ((Default != nullptr) && (Default->Bug[Index] == Bug[Index]))
    {
      Storage.DeleteValue(Key);
    }
    else
    {
      Storage.WriteInteger(Key, 2 - Bug[Index]);
    }
  }
  Storage.DeleteValue(QStringLiteral("BuggyMAC"));
  Storage.DeleteValue(QStringLiteral("AliasGroupList"));

  WRITE_DATA(String, SftpServer);
  WRITE_DATA_EX(Integer, "SFTPSymlinkBug", SFTPBug[sbSymlink]);
  WRITE_DATA_EX(Integer, "SFTPSignedTSBug", SFTPBug[sbSignedTS]);
  WRITE_DATA(Integer, SFTPMaxVersion);
  WRITE_DATA(Integer, SFTPMaxPacketSize);
  WRITE_DATA(Integer, SFTPDownloadQueue);
  WRITE_DATA(Integer, SFTPUploadQueue);
  WRITE_DATA(Integer, SFTPListingQueue);
  WRITE_DATA(Integer, SFTPRealPath);
  WRITE_DATA(Bool, UsePosixRename);

  WRITE_DATA(Integer, Color);

  WRITE_DATA(Bool, Tunnel);
  WRITE_DATA(String, TunnelHostName);
  WRITE_DATA(Integer, TunnelPortNumber);
  WRITE_DATA(String, TunnelUserName);
  WRITE_DATA(String, TunnelPublicKeyFile);
  WRITE_DATA(Integer, TunnelLocalPortNumber);
  if (TunnelHostKeyFromUrl)
  {
    Storage.DeleteValue(QStringLiteral("TunnelHostKey"));
  }
  else
  {
    WRITE_DATA(String, TunnelHostKey);
  }

  WRITE_DATA(Bool, FtpPasvMode);
  WRITE_DATA_EX(Integer, "FtpForcePasvIp2", FtpForcePasvIp);
  WRITE_DATA(Integer, FtpUseMlsd);
  WRITE_DATA(String, FtpAccount);
  WRITE_DATA(Integer, FtpPingInterval);
  WRITE_DATA(Integer, FtpPingType);
  WRITE_DATA_EX(Integer, "FtpTransferActiveImmediately2", FtpTransferActiveImmediately);
  WRITE_DATA(Integer, Ftps);
  WRITE_DATA(Integer, FtpListAll);
  WRITE_DATA(Integer, FtpHost);
  WRITE_DATA(Integer, FtpWorkFromCwd);
  WRITE_DATA(Bool, FtpAnyCodeForPwd);
  WRITE_DATA(Bool, SslSessionReuse);
  WRITE_DATA(String, TlsCertificateFile);
  WRITE_DATA(Integer, FtpProxyLogonType);
  WRITE_DATA(String, ProtocolFeatures);
  WRITE_DATA(Bool, NoExpansion);
  WRITE_DATA(Integer, MinTlsVersion);
  WRITE_DATA(Integer, MaxTlsVersion);
  WRITE_DATA(Integer, CompleteTlsShutdown);

  WRITE_DATA(Bool, WebDavLiberalEscaping);
  WRITE_DATA(Bool, WebDavAuthLegacy);

  WRITE_DATA(Bool, IsWorkspace);
  WRITE_DATA(Bool, NoteOnly);
  WRITE_DATA(String, Link);
  WRITE_DATA(String, NameOverride);

  #undef WRITE_DATA
  #undef WRITE_DATA_EX

  // TSessionData::SavePasswords
  Storage.DeleteValue(QStringLiteral("PasswordPlain"));
  if (Password.isEmpty() || DisablePasswordStoring)
  {
    Storage.DeleteValue(QStringLiteral("Password"));
  }
  else
  {
    Storage.WriteStringRaw(QStringLiteral("Password"), QString::fromLatin1(Security::EncryptPassword(Password, UserName + HostName)));
  }
  Storage.DeleteValue(QStringLiteral("ProxyPassword"));
  if (ProxyPassword.isEmpty())
  {
    Storage.DeleteValue(QStringLiteral("ProxyPasswordEnc"));
  }
  else
  {
    Storage.WriteStringRaw(QStringLiteral("ProxyPasswordEnc"), QString::fromLatin1(Security::EncryptPassword(ProxyPassword, ProxyUsername + ProxyHost)));
  }
  Storage.DeleteValue(QStringLiteral("TunnelPasswordPlain"));
  if (TunnelPassword.isEmpty() || DisablePasswordStoring)
  {
    Storage.DeleteValue(QStringLiteral("TunnelPassword"));
  }
  else
  {
    Storage.WriteStringRaw(QStringLiteral("TunnelPassword"), QString::fromLatin1(Security::EncryptPassword(TunnelPassword, TunnelUserName + TunnelHostName)));
  }
  Storage.DeleteValue(QStringLiteral("TunnelPassphrasePlain"));
  if (TunnelPassphrase.isEmpty() || DisablePasswordStoring)
  {
    Storage.DeleteValue(QStringLiteral("TunnelPassphrase"));
  }
  else
  {
    Storage.WriteStringRaw(QStringLiteral("TunnelPassphrase"), QString::fromLatin1(Security::EncryptPassword(TunnelPassphrase, TunnelPublicKeyFile)));
  }
}

QString SessionData::FolderName() const
{
  int P = Name.lastIndexOf(QLatin1Char('/'));
  return (P >= 0) ? Name.left(P) : QString();
}

QString SessionData::LocalName() const
{
  int P = Name.lastIndexOf(QLatin1Char('/'));
  return (P >= 0) ? Name.mid(P + 1) : Name;
}

QString SessionData::DefaultSessionName() const
{
  // TSessionData::GetDefaultSessionName
  QString Result = HostName;
  if (!UserName.isEmpty())
  {
    Result = UserName + QLatin1Char('@') + Result;
  }
  return Result;
}

QString SessionData::SessionName() const
{
  if (!Name.isEmpty() && (Name != DefaultName))
  {
    return Name;
  }
  return DefaultSessionName();
}

QString SessionData::ProtocolStr() const
{
  switch (FSProtocol)
  {
    case fsSCPonly: return QStringLiteral("SCP");
    case fsSFTP:
    case fsSFTPonly: return QStringLiteral("SFTP");
    case fsFTP: return (Ftps == ftpsNone) ? QStringLiteral("FTP") : QStringLiteral("FTPS");
    case fsWebDAV: return (Ftps == ftpsNone) ? QStringLiteral("WebDAV") : QStringLiteral("WebDAVS");
    case fsS3: return QStringLiteral("S3");
  }
  return QString();
}

QString SessionData::SessionUrl() const
{
  QString Scheme;
  switch (FSProtocol)
  {
    case fsSCPonly: Scheme = QStringLiteral("scp"); break;
    case fsSFTP: case fsSFTPonly: Scheme = QStringLiteral("sftp"); break;
    case fsFTP: Scheme = (Ftps == ftpsNone) ? QStringLiteral("ftp") : QStringLiteral("ftps"); break;
    case fsWebDAV: Scheme = (Ftps == ftpsNone) ? QStringLiteral("dav") : QStringLiteral("davs"); break;
    case fsS3: Scheme = QStringLiteral("s3"); break;
  }
  QUrl Url;
  Url.setScheme(Scheme);
  Url.setHost(HostName);
  Url.setUserName(UserName);
  if (PortNumber != DefaultPort(FSProtocol, Ftps))
  {
    Url.setPort(PortNumber);
  }
  Url.setPath(QStringLiteral("/"));
  return Url.toString();
}

int SessionData::DefaultPort(TFSProtocol Protocol, TFtps Ftps)
{
  switch (Protocol)
  {
    case fsFTP: return (Ftps == ftpsImplicit) ? FtpsImplicitPortNumber : FtpPortNumber;
    case fsWebDAV:
    case fsS3: return (Ftps == ftpsNone) ? HTTPPortNumber : HTTPSPortNumber;
    default: return SshPortNumber;
  }
}

//---------------------------------------------------------------------------

QString SessionData::InfoTip() const
{
  if (NoteOnly)
  {
    return QStringLiteral("Note");
  }
  // TSessionData::GetInfoTip; FSProtocolNames
  QString ProtocolName;
  switch (FSProtocol)
  {
    case fsSCPonly: ProtocolName = QStringLiteral("SCP"); break;
    case fsSFTP: ProtocolName = QStringLiteral("SFTP (SCP)"); break;
    case fsSFTPonly: ProtocolName = QStringLiteral("SFTP"); break;
    case fsFTP: ProtocolName = QStringLiteral("FTP"); break;
    case fsWebDAV: ProtocolName = QStringLiteral("WebDAV"); break;
    case fsS3: ProtocolName = QStringLiteral("S3"); break;
  }
  if (IsSsh())
  {
    // SESSION_INFO_TIP2
    return QStringLiteral("Host: %1\nUsername: %2\nPrivate key file: %3\nTransfer protocol: %4").arg(
      HostName, UserName, PublicKeyFile.isEmpty() ? QStringLiteral("No") : QStringLiteral("Yes"), ProtocolName);
  }
  // SESSION_INFO_TIP_NO_SSH
  return QStringLiteral("Host: %1\nUsername: %2\nTransfer protocol: %3").arg(HostName, UserName, ProtocolName);
}

void SessionData::MakeUniqueIn(const StoredSessionList & List)
{
  // TNamedObject::MakeUniqueIn
  while (List.FindByName(Name) != nullptr)
  {
    int N = 0;
    qsizetype P;
    // If name already contains number parenthesis remove it (and remember it)
    if (Name.endsWith(QLatin1Char(')')) && ((P = Name.lastIndexOf(QLatin1Char('('))) >= 0))
    {
      bool Ok = false;
      N = Name.mid(P + 1, Name.size() - P - 2).toInt(&Ok);
      if (Ok)
      {
        Name = Name.left(P);
        while (Name.endsWith(QLatin1Char(' ')))
        {
          Name.chop(1);
        }
      }
      else
      {
        N = 0;
      }
    }
    Name += QStringLiteral(" (%1)").arg(N + 1);
  }
}

int SessionData::RgbToColor(unsigned int Rgb)
{
  int Result = static_cast<int>(((Rgb >> 16) & 0xFF) | (Rgb & 0xFF00) | ((Rgb & 0xFF) << 16));
  // 0 means "no color", black is stored as almost black
  return (Result == 0) ? 0x010101 : Result;
}

QString SessionData::NormalizedPuttyProtocol() const
{
  QString Result = FRawValues.value(QStringLiteral("Protocol"));
  return Result.isEmpty() ? QStringLiteral("ssh") : Result;
}

bool SessionData::IsValidName(const QString & Name, QString * Error)
{
  // keep consistent with MakeValidName
  if (Name.contains(QLatin1Char('/')))
  {
    if (Error != nullptr)
    {
      // ITEM_NAME_INVALID
      *Error = QStringLiteral("Invalid name '%1'. Name cannot contain '%2'.").arg(Name, QStringLiteral("/"));
    }
    return false;
  }
  return true;
}

QString SessionData::MakeValidName(const QString & Name)
{
  // keep consistent with IsValidName
  return QString(Name).replace(QLatin1Char('/'), QLatin1Char('\\'));
}

QString SessionData::ComposePath(const QString & Folder, const QString & Name)
{
  if (Folder.isEmpty())
  {
    return Name;
  }
  return Folder.endsWith(QLatin1Char('/')) ? (Folder + Name) : (Folder + QLatin1Char('/') + Name);
}

static QString EnvironmentVariable(const QString & Name)
{
  if (qEnvironmentVariableIsSet(Name.toLocal8Bit().constData()))
  {
    return qEnvironmentVariable(Name.toLocal8Bit().constData());
  }
  // Windows variables commonly found in imported WinSCP.ini files
  QString Upper = Name.toUpper();
  if ((Upper == QStringLiteral("USERPROFILE")) || (Upper == QStringLiteral("HOME")))
  {
    return QDir::homePath();
  }
  if (Upper == QStringLiteral("APPDATA"))
  {
    QString Config = qEnvironmentVariable("XDG_CONFIG_HOME");
    return Config.isEmpty() ? (QDir::homePath() + QStringLiteral("/.config")) : Config;
  }
  if (Upper == QStringLiteral("LOCALAPPDATA"))
  {
    QString Data = qEnvironmentVariable("XDG_DATA_HOME");
    return Data.isEmpty() ? (QDir::homePath() + QStringLiteral("/.local/share")) : Data;
  }
  if ((Upper == QStringLiteral("TEMP")) || (Upper == QStringLiteral("TMP")))
  {
    return QDir::tempPath();
  }
  if ((Upper == QStringLiteral("USERNAME")) && qEnvironmentVariableIsSet("USER"))
  {
    return qEnvironmentVariable("USER");
  }
  return QString();
}

static bool IsVariableNameChar(QChar C)
{
  return C.isLetterOrNumber() || (C == QLatin1Char('_'));
}

QString SessionData::ExpandEnvironmentVariables(const QString & Str)
{
  QString Result;
  qsizetype I = 0;
  if ((Str == QStringLiteral("~")) || Str.startsWith(QStringLiteral("~/")))
  {
    Result = QDir::homePath();
    I = 1;
  }
  while (I < Str.size())
  {
    QChar C = Str[I];
    if (C == QLatin1Char('%'))
    {
      qsizetype E = Str.indexOf(QLatin1Char('%'), I + 1);
      if (E > I + 1)
      {
        QString Value = EnvironmentVariable(Str.mid(I + 1, E - I - 1));
        if (!Value.isNull())
        {
          Result += Value;
          I = E + 1;
          continue;
        }
      }
    }
    else if ((C == QLatin1Char('$')) && (I + 1 < Str.size()))
    {
      if (Str[I + 1] == QLatin1Char('{'))
      {
        qsizetype E = Str.indexOf(QLatin1Char('}'), I + 2);
        if (E > I + 2)
        {
          QString Value = EnvironmentVariable(Str.mid(I + 2, E - I - 2));
          if (!Value.isNull())
          {
            Result += Value;
            I = E + 1;
            continue;
          }
        }
      }
      else if (Str[I + 1].isLetter() || (Str[I + 1] == QLatin1Char('_')))
      {
        qsizetype E = I + 1;
        while ((E < Str.size()) && IsVariableNameChar(Str[E]))
        {
          E++;
        }
        QString Value = EnvironmentVariable(Str.mid(I + 1, E - I - 1));
        if (!Value.isNull())
        {
          Result += Value;
          I = E;
          continue;
        }
      }
    }
    Result += C;
    I++;
  }
  return Result;
}

void SessionData::ExpandEnvironmentVariables()
{
  if (NoExpansion)
  {
    return;
  }
  // TSessionData::ExpandEnvironmentVariables, plus the other local paths
  // WinSCP reads through Get*Expanded
  HostName = ExpandEnvironmentVariables(HostName);
  UserName = ExpandEnvironmentVariables(UserName);
  PublicKeyFile = ExpandEnvironmentVariables(PublicKeyFile);
  DetachedCertificate = ExpandEnvironmentVariables(DetachedCertificate);
  TunnelHostName = ExpandEnvironmentVariables(TunnelHostName);
  TunnelUserName = ExpandEnvironmentVariables(TunnelUserName);
  TunnelPublicKeyFile = ExpandEnvironmentVariables(TunnelPublicKeyFile);
  TlsCertificateFile = ExpandEnvironmentVariables(TlsCertificateFile);
  LocalDirectory = ExpandEnvironmentVariables(LocalDirectory);
  OtherLocalDirectory = ExpandEnvironmentVariables(OtherLocalDirectory);
}

void SessionData::ProtectFromExpansion(const SessionData & Base)
{
  for (QString SessionData::* Field : {&SessionData::HostName, &SessionData::UserName, &SessionData::PublicKeyFile,
         &SessionData::DetachedCertificate, &SessionData::TunnelHostName, &SessionData::TunnelUserName,
         &SessionData::TunnelPublicKeyFile, &SessionData::TlsCertificateFile, &SessionData::LocalDirectory,
         &SessionData::OtherLocalDirectory})
  {
    if (!Base.NoExpansion && (this->*Field == Base.*Field))
    {
      this->*Field = ExpandEnvironmentVariables(this->*Field);
    }
  }
  NoExpansion = true;
}

bool SessionData::IsS3HostName(const QString & HostName)
{
  // IsDomainOrSubdomain, as TSessionData::ParseUrl. Any amazonaws.com host
  // rather than only s3.amazonaws.com, so that regional endpoints are included.
  auto IsDomainOrSubdomain = [&HostName](const QString & Domain)
  {
    return (HostName.compare(Domain, Qt::CaseInsensitive) == 0) ||
      HostName.endsWith(QLatin1Char('.') + Domain, Qt::CaseInsensitive);
  };
  return
    IsDomainOrSubdomain(QStringLiteral("amazonaws.com")) ||
    IsDomainOrSubdomain(QStringLiteral("digitaloceanspaces.com")) ||
    IsDomainOrSubdomain(QStringLiteral("storage.googleapis.com")) ||
    IsDomainOrSubdomain(QStringLiteral("r2.cloudflarestorage.com")) ||
    (IsDomainOrSubdomain(QStringLiteral("oraclecloud.com")) && HostName.contains(QStringLiteral(".compat.objectstorage."), Qt::CaseInsensitive));
}

static bool IsHexChar(QChar C)
{
  return C.isDigit() || ((C.toLower() >= QLatin1Char('a')) && (C.toLower() <= QLatin1Char('f')));
}

// DecodeUrlChars
static QString DecodeUrlChars(QString S)
{
  qsizetype I = 0;
  while (I < S.size())
  {
    if (S[I] == QLatin1Char('+'))
    {
      S[I] = QLatin1Char(' ');
    }
    else if (S[I] == QLatin1Char('%'))
    {
      QByteArray Bytes;
      while ((I + 2 < S.size()) && (S[I] == QLatin1Char('%')) && IsHexChar(S[I + 1]) && IsHexChar(S[I + 2]))
      {
        Bytes += QByteArray::fromHex(S.mid(I + 1, 2).toLatin1());
        S.remove(I, 3);
      }
      if (!Bytes.isEmpty())
      {
        QString Chars = QString::fromUtf8(Bytes);
        S.insert(I, Chars);
        I += Chars.size() - 1;
      }
    }
    I++;
  }
  return S;
}

static QString CutToChar(QString & Str, QChar Ch, bool Trim)
{
  qsizetype P = Str.indexOf(Ch);
  QString Result;
  if (P >= 0)
  {
    Result = Str.left(P);
    Str.remove(0, P + 1);
  }
  else
  {
    Result = Str;
    Str.clear();
  }
  if (Trim)
  {
    Result = Result.trimmed();
    Str = Str.trimmed();
  }
  return Result;
}

static bool IsProtocolUrl(const QString & Url, const QString & Protocol, int & ProtocolLen)
{
  // DoIsProtocolUrl (also "winscp-protocol:")
  for (const QString & Prefix : {Protocol + QLatin1Char(':'), QStringLiteral("winscp-") + Protocol + QLatin1Char(':')})
  {
    if (Url.startsWith(Prefix, Qt::CaseInsensitive))
    {
      ProtocolLen = static_cast<int>(Prefix.size());
      return true;
    }
  }
  return false;
}

bool SessionData::IsIPv6Literal(const QString & HostName)
{
  // IsIPv6Literal: two or more colons (optionally in brackets)
  QString Host = HostName;
  if (Host.startsWith(QLatin1Char('[')) && Host.endsWith(QLatin1Char(']')))
  {
    Host = Host.mid(1, Host.size() - 2);
  }
  if (Host.count(QLatin1Char(':')) < 2)
  {
    return false;
  }
  for (QChar C : Host)
  {
    if (!IsHexChar(C) && (C != QLatin1Char(':')) && (C != QLatin1Char('.')) && (C != QLatin1Char('%')))
    {
      return false;
    }
  }
  return true;
}

bool SessionData::ParseUrl(const QString & AUrl, int & ParsedInfo)
{
  // TSessionData::ParseUrl with pufPreferProtocol, no stored sites, no options
  const SessionData Original = *this;
  QString Url = AUrl.trimmed();
  ParsedInfo = 0;
  bool ProtocolDefined = true;
  TFSProtocol AFSProtocol = fsSFTP;
  int DefaultProtocolPortNumber = 0;
  TFtps AFtps = ftpsNone;
  int ProtocolLen = 0;
  bool HttpForWebdav = (FSProtocol != fsS3);
  if (IsProtocolUrl(Url, QStringLiteral("scp"), ProtocolLen))
  {
    AFSProtocol = fsSCPonly;
    DefaultProtocolPortNumber = SshPortNumber;
  }
  else if (IsProtocolUrl(Url, QStringLiteral("sftp"), ProtocolLen))
  {
    AFSProtocol = fsSFTPonly;
    DefaultProtocolPortNumber = SshPortNumber;
  }
  else if (IsProtocolUrl(Url, QStringLiteral("ftp"), ProtocolLen))
  {
    AFSProtocol = fsFTP;
    DefaultProtocolPortNumber = FtpPortNumber;
  }
  else if (IsProtocolUrl(Url, QStringLiteral("ftps"), ProtocolLen))
  {
    AFSProtocol = fsFTP;
    AFtps = ftpsImplicit;
    DefaultProtocolPortNumber = FtpsImplicitPortNumber;
  }
  else if (IsProtocolUrl(Url, QStringLiteral("ftpes"), ProtocolLen))
  {
    AFSProtocol = fsFTP;
    AFtps = ftpsExplicitTls;
    DefaultProtocolPortNumber = FtpPortNumber;
  }
  else if (IsProtocolUrl(Url, QStringLiteral("dav"), ProtocolLen) ||
           (HttpForWebdav && IsProtocolUrl(Url, QStringLiteral("http"), ProtocolLen)))
  {
    AFSProtocol = fsWebDAV;
    DefaultProtocolPortNumber = HTTPPortNumber;
  }
  else if (IsProtocolUrl(Url, QStringLiteral("davs"), ProtocolLen) ||
           (HttpForWebdav && IsProtocolUrl(Url, QStringLiteral("https"), ProtocolLen)))
  {
    AFSProtocol = fsWebDAV;
    AFtps = ftpsImplicit;
    DefaultProtocolPortNumber = HTTPSPortNumber;
  }
  else if (IsProtocolUrl(Url, QStringLiteral("s3plain"), ProtocolLen) ||
           IsProtocolUrl(Url, QStringLiteral("http"), ProtocolLen))
  {
    AFSProtocol = fsS3;
    DefaultProtocolPortNumber = HTTPPortNumber;
  }
  else if (IsProtocolUrl(Url, QStringLiteral("s3"), ProtocolLen) ||
           IsProtocolUrl(Url, QStringLiteral("https"), ProtocolLen))
  {
    AFSProtocol = fsS3;
    AFtps = ftpsImplicit;
    DefaultProtocolPortNumber = HTTPSPortNumber;
  }
  else if (IsProtocolUrl(Url, QStringLiteral("ssh"), ProtocolLen))
  {
    AFSProtocol = fsSFTPonly;
    DefaultProtocolPortNumber = SshPortNumber;
  }
  else
  {
    ProtocolDefined = false;
  }

  if (ProtocolDefined)
  {
    ParsedInfo |= ParsedProtocolDefined;
    Url.remove(0, ProtocolLen);
    if (Url.startsWith(QStringLiteral("//")))
    {
      Url.remove(0, 2);
    }
  }

  if (Url.isEmpty())
  {
    ParsedInfo |= ParsedDefaultsOnly;
  }
  else
  {
    qsizetype PSlash = Url.indexOf(QLatin1Char('/'));
    if (PSlash < 0)
    {
      PSlash = Url.size();
    }
    QString ConnectInfo = Url.left(PSlash);
    qsizetype P = ConnectInfo.lastIndexOf(QLatin1Char('@'));
    QString UserInfo;
    QString HostInfo;
    if (P >= 0)
    {
      UserInfo = ConnectInfo.left(P);
      HostInfo = ConnectInfo.mid(P + 1);
    }
    else
    {
      HostInfo = ConnectInfo;
    }

    if ((HostInfo.size() >= 2) && (HostInfo[0] == QLatin1Char('[')) && ((P = HostInfo.indexOf(QLatin1Char(']'))) > 0))
    {
      HostName = HostInfo.mid(1, P - 1);
      HostInfo.remove(0, P + 1);
      if (HostInfo.startsWith(QLatin1Char(':')))
      {
        HostInfo.remove(0, 1);
      }
    }
    else
    {
      HostName = DecodeUrlChars(CutToChar(HostInfo, QLatin1Char(':'), true));
    }

    if (!HostInfo.isEmpty())
    {
      bool Ok = false;
      int APortNumber = DecodeUrlChars(HostInfo).toInt(&Ok);
      if (Ok && (APortNumber > 0) && (APortNumber <= 65535))
      {
        PortNumber = APortNumber;
      }
    }
    else if (ProtocolDefined)
    {
      if ((AFSProtocol == fsWebDAV) && IsS3HostName(HostName))
      {
        AFSProtocol = fsS3;
      }
      PortNumber = DefaultProtocolPortNumber;
    }

    if (ProtocolDefined)
    {
      Ftps = AFtps;
    }

    QString UserInfoWithoutConnectionParams = CutToChar(UserInfo, QLatin1Char(';'), false);
    QString ConnectionParams = UserInfo;
    UserInfo = UserInfoWithoutConnectionParams;
    QStringList RawSettings;
    while (!ConnectionParams.isEmpty())
    {
      QString ConnectionParam = CutToChar(ConnectionParams, QLatin1Char(';'), false);
      QString ConnectionParamName = CutToChar(ConnectionParam, QLatin1Char('='), false);
      if (ConnectionParamName.compare(QStringLiteral("fingerprint"), Qt::CaseInsensitive) == 0)
      {
        HostKey = UrlHostKeys(DecodeUrlChars(ConnectionParam));
        HostKeyFromUrl = true;
      }
      else if (ConnectionParamName.startsWith(QStringLiteral("x-"), Qt::CaseInsensitive))
      {
        RawSettings.append(DecodeUrlChars(ConnectionParamName.mid(2)) + QLatin1Char('=') + DecodeUrlChars(ConnectionParam));
      }
    }
    if (!RawSettings.isEmpty() && ApplyUrlSettings(RawSettings))
    {
      ParsedInfo |= ParsedUnsafeSettings;
    }

    UserName = DecodeUrlChars(CutToChar(UserInfo, QLatin1Char(':'), false));
    Password = DecodeUrlChars(UserInfo);

    QString RemoteDirectoryWithSessionParams = Url.mid(PSlash);
    QString ARemoteDirectory = CutToChar(RemoteDirectoryWithSessionParams, QLatin1Char(';'), false);
    if (!ARemoteDirectory.isEmpty() && (ARemoteDirectory != QStringLiteral("/")))
    {
      RemoteDirectory = DecodeUrlChars(ARemoteDirectory);
    }
  }

  if (ProtocolDefined)
  {
    FSProtocol = AFSProtocol;
  }
  // What the URL has set is never expanded (host and user names go to the server)
  ProtectFromExpansion(Original);
  return true;
}

// Values stored with WriteStringRaw (not escaped)
static bool IsRawSettingKey(const QString & Key)
{
  static const QStringList RawKeys = {
    QStringLiteral("ProxyTelnetCommand"), QStringLiteral("TimeDifference"), QStringLiteral("Password"),
    QStringLiteral("ProxyPasswordEnc"), QStringLiteral("TunnelPassword"), QStringLiteral("TunnelPassphrase"),
    QStringLiteral("EncryptKey") };
  return RawKeys.contains(Key, Qt::CaseInsensitive);
}

QStringList SessionData::RawSettingsForUrl() const
{
  SessionData FactoryDefaults;
  SessionData Copy = *this;
  Copy.FSProtocol = FactoryDefaults.FSProtocol;
  Copy.HostName = FactoryDefaults.HostName;
  Copy.PortNumber = FactoryDefaults.PortNumber;
  Copy.UserName = FactoryDefaults.UserName;
  Copy.Password = FactoryDefaults.Password;
  Copy.Ftps = FactoryDefaults.Ftps;
  Copy.HostKey = FactoryDefaults.HostKey;
  // TSessionData::ResetNonCoreData
  Copy.RemoteDirectory = FactoryDefaults.RemoteDirectory;
  Copy.LocalDirectory = FactoryDefaults.LocalDirectory;
  Copy.OtherLocalDirectory = FactoryDefaults.OtherLocalDirectory;
  Copy.SynchronizeBrowsing = FactoryDefaults.SynchronizeBrowsing;
  Copy.Color = FactoryDefaults.Color;
  Copy.UpdateDirectories = FactoryDefaults.UpdateDirectories;
  Copy.Note = FactoryDefaults.Note;
  Copy.AgentAccess = FactoryDefaults.AgentAccess;
  Copy.AgentPermissions = FactoryDefaults.AgentPermissions;
  Copy.AgentName = FactoryDefaults.AgentName;
  Copy.AgentAllow = FactoryDefaults.AgentAllow;
  Copy.AgentDeny = FactoryDefaults.AgentDeny;
  Copy.AgentRecord = FactoryDefaults.AgentRecord;
  Copy.PortForwards = FactoryDefaults.PortForwards;
  Copy.NoExpansion = FactoryDefaults.NoExpansion;
  QStringList Result = Copy.SaveToOptions(&FactoryDefaults);
  // SaveName: HasSessionName() && (Name != DefaultSessionName)
  if (!Name.isEmpty() && (Name != DefaultSessionName()) && !IsWorkspace)
  {
    bool HasName = false;
    for (const QString & S : Result)
    {
      HasName = HasName || S.startsWith(QStringLiteral("Name="), Qt::CaseInsensitive);
    }
    if (!HasName)
    {
      Result.prepend(QStringLiteral("Name=") + Name);
    }
  }
  return Result;
}

QStringList SessionData::SaveToOptions(const SessionData * Default) const
{
  IniFile File;
  IniStorage Storage(&File);
  Storage.OpenSubKey(QStringLiteral("Options"), true);
  Save(Storage, Default);
  QStringList Result;
  for (const QString & Key : Storage.GetValueNames())
  {
    QString Value;
    QString OutKey = Key;
    // DoNotEncryptPasswords
    if (Key == QStringLiteral("Password"))
    {
      OutKey = QStringLiteral("PasswordPlain");
      Value = Password;
    }
    else if (Key == QStringLiteral("ProxyPasswordEnc"))
    {
      OutKey = QStringLiteral("ProxyPassword");
      Value = ProxyPassword;
    }
    else if (Key == QStringLiteral("TunnelPassword"))
    {
      OutKey = QStringLiteral("TunnelPasswordPlain");
      Value = TunnelPassword;
    }
    else if (Key == QStringLiteral("TunnelPassphrase"))
    {
      OutKey = QStringLiteral("TunnelPassphrasePlain");
      Value = TunnelPassphrase;
    }
    else if (IsRawSettingKey(Key))
    {
      Value = Storage.ReadStringRaw(Key, QString());
    }
    else
    {
      Value = Storage.ReadString(Key, QString());
    }
    Result.append(OutKey + QLatin1Char('=') + Value);
  }
  return Result;
}

bool SessionData::IsUrlSetting(const QString & Key)
{
  static const QStringList Keys = {
    // connection
    QStringLiteral("PingType"), QStringLiteral("PingInterval"), QStringLiteral("PingIntervalSecs"), QStringLiteral("PingIntervalSec"),
    QStringLiteral("Timeout"), QStringLiteral("AddressFamily"), QStringLiteral("TcpNoDelay"), QStringLiteral("SendBuf"),
    QStringLiteral("SshSendBuf"), QStringLiteral("SshSimple"), QStringLiteral("Compression"), QStringLiteral("RekeyBytes"),
    QStringLiteral("RekeyTime"),
    // directories and listings
    QStringLiteral("UpdateDirectories"), QStringLiteral("CacheDirectories"), QStringLiteral("CacheDirectoryChanges"),
    QStringLiteral("PreserveDirectoryChanges"), QStringLiteral("ResolveSymlinks"), QStringLiteral("FollowDirectorySymlinks"),
    QStringLiteral("ConsiderDST"), QStringLiteral("TimeDifference"), QStringLiteral("TimeDifferenceAuto"), QStringLiteral("EOLType"),
    QStringLiteral("TrimVMSVersions"), QStringLiteral("VMSAllRevisions"), QStringLiteral("Utf"), QStringLiteral("SFTPUtfBug"),
    // SCP
    QStringLiteral("ClearAliases"), QStringLiteral("UnsetNationalVars"), QStringLiteral("IgnoreLsWarnings"),
    QStringLiteral("SCPLsFullTime"), QStringLiteral("Scp1Compatibility"), QStringLiteral("LookupUserGroups2"),
    QStringLiteral("ExitCode1IsError"),
    // SFTP
    QStringLiteral("SFTPSymlinkBug"), QStringLiteral("SFTPSignedTSBug"), QStringLiteral("SFTPMaxVersion"),
    QStringLiteral("SFTPMaxPacketSize"), QStringLiteral("SFTPDownloadQueue"), QStringLiteral("SFTPUploadQueue"),
    QStringLiteral("SFTPListingQueue"), QStringLiteral("SFTPRealPath"), QStringLiteral("UsePosixRename"),
    // FTP
    QStringLiteral("FtpPasvMode"), QStringLiteral("FtpUseMlsd"), QStringLiteral("FtpPingInterval"), QStringLiteral("FtpPingType"),
    QStringLiteral("FtpTransferActiveImmediately2"), QStringLiteral("FtpListAll"), QStringLiteral("FtpHost"),
    QStringLiteral("FtpWorkFromCwd"), QStringLiteral("FtpDeleteFromCwd"), QStringLiteral("FtpAnyCodeForPwd"),
    // WebDAV and S3
    QStringLiteral("WebDavLiberalEscaping"), QStringLiteral("S3DefaultRegion"), QStringLiteral("S3UrlStyle"), QStringLiteral("S3MaxKeys") };
  return Keys.contains(Key.trimmed(), Qt::CaseInsensitive);
}

QString SessionData::UrlHostKeys(const QString & Keys)
{
  QStringList Result;
  for (const QString & Entry : Keys.split(QLatin1Char(';'), Qt::SkipEmptyParts))
  {
    QString Key = Entry.trimmed();
    if ((Key != QStringLiteral("*")) && (Key.compare(QStringLiteral("acceptnew"), Qt::CaseInsensitive) != 0))
    {
      Result.append(Key);
    }
  }
  return Result.join(QLatin1Char(';'));
}

QString SessionData::TrustedHostKey() const
{
  return HostKeyFromUrl ? UrlHostKeys(HostKey) : HostKey;
}

bool SessionData::ApplyUrlSettings(const QStringList & Settings)
{
  QString OldHostKey = HostKey;
  QString OldTunnelHostKey = TunnelHostKey;
  bool Unsafe = ApplyRawSettings(Settings);
  for (const QString & Setting : Settings)
  {
    Unsafe = Unsafe || !IsUrlSetting(Setting.left(Setting.indexOf(QLatin1Char('='))));
  }
  if (HostKey != OldHostKey)
  {
    HostKey = UrlHostKeys(HostKey);
    HostKeyFromUrl = true;
  }
  if (TunnelHostKey != OldTunnelHostKey)
  {
    TunnelHostKey = UrlHostKeys(TunnelHostKey);
    TunnelHostKeyFromUrl = true;
  }
  return Unsafe;
}

bool SessionData::ApplyRawSettings(const QStringList & Settings)
{
  // TSessionData::ApplyRawSettings over a TOptionsStorage
  IniFile File;
  IniStorage Storage(&File);
  Storage.OpenSubKey(QStringLiteral("Options"), true);
  bool UnsafeKey = false;
  for (const QString & Setting : Settings)
  {
    qsizetype P = Setting.indexOf(QLatin1Char('='));
    QString Key = ((P >= 0) ? Setting.left(P) : Setting).trimmed();
    QString Value = (P >= 0) ? Setting.mid(P + 1) : QString();
    if (Key.isEmpty())
    {
      continue;
    }
    if ((Key.compare(QStringLiteral("GSSLibs"), Qt::CaseInsensitive) == 0) ||
        (Key.compare(QStringLiteral("WebDavCrossDomainRedirects"), Qt::CaseInsensitive) == 0) ||
        (Key.compare(QStringLiteral("WebDavUnencryptedRedirects"), Qt::CaseInsensitive) == 0))
    {
      UnsafeKey = true;
    }
    if (IsRawSettingKey(Key))
    {
      Storage.WriteStringRaw(Key, Value);
    }
    else
    {
      Storage.WriteString(Key, Value);
    }
  }
  SessionData Before = *this;
  QMap<QString, QString> OldRawValues = FRawValues;
  QString AName = Name;
  Load(Storage);
  Name = AName;
  // The trust store of the session is not for a URL or raw settings to choose
  KnownHostsFile = Before.KnownHostsFile;
  LogicalHostName = Before.LogicalHostName;
  NoExpansion = Before.NoExpansion;
  // Agent access is granted in NixSCP only, never by a URL or raw settings
  AgentAccess = Before.AgentAccess;
  AgentPermissions = Before.AgentPermissions;
  AgentName = Before.AgentName;
  AgentAllow = Before.AgentAllow;
  AgentDeny = Before.AgentDeny;
  AgentRecord = Before.AgentRecord;
  PortForwards = Before.PortForwards;
  // What is written here is meant: a key of the URL it replaces is no longer one
  HostKeyFromUrl = HostKeyFromUrl && (HostKey == Before.HostKey);
  TunnelHostKeyFromUrl = TunnelHostKeyFromUrl && (TunnelHostKey == Before.TunnelHostKey);
  for (auto It = OldRawValues.cbegin(); It != OldRawValues.cend(); ++It)
  {
    if (!FRawValues.contains(It.key()))
    {
      FRawValues.insert(It.key(), It.value());
    }
  }
  // LOADING_UNSAFE properties
  return UnsafeKey ||
    (Before.Shell != Shell) || (Before.ListingCommand != ListingCommand) ||
    (Before.DeleteToRecycleBin != DeleteToRecycleBin) || (Before.OverwrittenToRecycleBin != OverwrittenToRecycleBin) ||
    (Before.RecycleBinPath != RecycleBinPath) || (Before.PostLoginCommands != PostLoginCommands) ||
    (Before.ReturnVar != ReturnVar) || (Before.SftpServer != SftpServer) ||
    (Before.ProxyTelnetCommand != ProxyTelnetCommand) || (Before.ProxyLocalCommand != ProxyLocalCommand);
}

//---------------------------------------------------------------------------

StoredSessionList::StoredSessionList() :
  FDefaultSettings(std::make_shared<SessionData>())
{
  FDefaultSettings->Name = DefaultName;
}

QString StoredSessionList::DefaultSettingsName()
{
  return DefaultName;
}

void StoredSessionList::Load(IniFile & File)
{
  FSessions.clear();
  FDeleted.clear();
  IniStorage Storage(&File);
  if (Storage.OpenSubKey(SessionsKey, false))
  {
    // TStoredSessionList::Load: sites inherit values not stored in their
    // section from "Default Settings", so load that first
    QStringList Names = Storage.GetSubKeyNames();
    auto Defaults = std::make_shared<SessionData>();
    Defaults->Name = DefaultName;
    if (Names.contains(DefaultName) && Storage.OpenSubKey(DefaultName, false))
    {
      Defaults->Load(Storage);
      Storage.CloseSubKey();
    }
    FDefaultSettings = Defaults;
    for (const QString & SessionName : Names)
    {
      if (SessionName == DefaultName)
      {
        continue;
      }
      auto Data = std::make_shared<SessionData>(*FDefaultSettings);
      if (Storage.OpenSubKey(SessionName, false))
      {
        Data->Load(Storage);
        Storage.CloseSubKey();
      }
      Data->Name = SessionName;
      FSessions.append(Data);
    }
    Storage.CloseSubKey();
  }
  std::sort(FSessions.begin(), FSessions.end(),
    [](const auto & A, const auto & B) { return A->Name.compare(B->Name, Qt::CaseInsensitive) < 0; });
}

void StoredSessionList::Save(IniFile & File) const
{
  IniStorage Storage(&File);
  if (Storage.OpenSubKey(SessionsKey, true))
  {
    for (const QString & Name : FDeleted)
    {
      Storage.RecursiveDeleteSubKey(Name);
    }
    // TStoredSessionList::Save: "Default Settings" relative to factory
    // defaults, sites relative to "Default Settings"
    SessionData FactoryDefaults;
    if (Storage.OpenSubKey(DefaultName, true))
    {
      FDefaultSettings->Save(Storage, &FactoryDefaults);
      Storage.CloseSubKey();
    }
    for (const auto & Data : FSessions)
    {
      // Rewrite the section from scratch, so renamed/removed keys do not linger
      Storage.RecursiveDeleteSubKey(Data->Name);
      if (Storage.OpenSubKey(Data->Name, true))
      {
        Data->Save(Storage, FDefaultSettings.get());
        Storage.CloseSubKey();
      }
    }
    Storage.CloseSubKey();
  }
}

int StoredSessionList::ImportFromWinSCPIni(const QString & FileName, QString * Error)
{
  IniFile File;
  if (!File.Load(FileName))
  {
    if (Error) *Error = QStringLiteral("Cannot open file \"%1\".").arg(FileName);
    return 0;
  }
  StoredSessionList Imported;
  Imported.Load(File);
  int Count = 0;
  for (const auto & Data : Imported.Sessions())
  {
    if (Data->IsWorkspace)
    {
      continue;
    }
    // Do not overwrite existing sites, as WinSCP import does not either
    QString Name = Data->Name;
    int Index = 2;
    while (FindByName(Name))
    {
      Name = QStringLiteral("%1 (%2)").arg(Data->Name).arg(Index++);
    }
    Data->Name = Name;
    Add(Data);
    Count++;
  }
  return Count;
}

int StoredSessionList::CompareLogicalText(const QString & A, const QString & B)
{
  // CompareLogicalText with NaturalOrderNumericalSorting (StrCmpLogicalW)
  // Digit runs are compared by value here: QCollator's numeric mode does
  // nothing in the C/POSIX locale (and without ICU)
  static thread_local QCollator Collator = []()
  {
    QCollator C;
    C.setCaseSensitivity(Qt::CaseInsensitive);
    return C;
  }();
  // Without ICU (Android) the collator compares code points and ignores the
  // case setting, so uppercase sorts first: the text is folded by hand there
  static thread_local const bool Collating =
    !qEnvironmentVariableIsSet("NIXSCP_NO_COLLATOR") && (Collator.compare(QStringLiteral("a"), QStringLiteral("B")) < 0);
  auto IsAscii = [](QStringView Text)
  {
    return std::all_of(Text.begin(), Text.end(), [](QChar C) { return C.unicode() < 0x80; });
  };
  // the letter without its case and accents: "Č" as "c", "Đ" (which has no decomposition) as "d"
  auto Folded = [](QStringView Text)
  {
    QString Result = Text.toString().normalized(QString::NormalizationForm_D);
    Result.removeIf([](QChar C) { return C.category() == QChar::Mark_NonSpacing; });
    return Result.toCaseFolded().replace(QChar(0x0111), QLatin1Char('d'));
  };
  auto CompareText = [&](QStringView X, QStringView Y) -> int
  {
    if (Collating)
    {
      return Collator.compare(X, Y);
    }
    return (IsAscii(X) && IsAscii(Y)) ? X.compare(Y, Qt::CaseInsensitive) : QStringView(Folded(X)).compare(Folded(Y));
  };
  auto IsDigit = [](QChar C) { return (C >= QLatin1Char('0')) && (C <= QLatin1Char('9')); };
  int Result = 0;
  qsizetype IA = 0, IB = 0;
  while ((Result == 0) && (IA < A.size()) && (IB < B.size()))
  {
    bool DigitA = IsDigit(A[IA]);
    qsizetype EA = IA, EB = IB;
    while ((EA < A.size()) && (IsDigit(A[EA]) == DigitA)) EA++;
    while ((EB < B.size()) && (IsDigit(B[EB]) == IsDigit(B[IB]))) EB++;
    if (DigitA && IsDigit(B[IB]))
    {
      QStringView NA = QStringView(A).sliced(IA, EA - IA), NB = QStringView(B).sliced(IB, EB - IB);
      while ((NA.size() > 1) && (NA[0] == QLatin1Char('0'))) NA = NA.sliced(1);
      while ((NB.size() > 1) && (NB[0] == QLatin1Char('0'))) NB = NB.sliced(1);
      Result = (NA.size() != NB.size()) ? ((NA.size() < NB.size()) ? -1 : 1) : NA.compare(NB);
    }
    else if (DigitA != IsDigit(B[IB]))
    {
      Result = CompareText(QStringView(A).sliced(IA), QStringView(B).sliced(IB));
      if (Result == 0)
      {
        Result = DigitA ? -1 : 1;
      }
    }
    else
    {
      Result = CompareText(QStringView(A).sliced(IA, EA - IA), QStringView(B).sliced(IB, EB - IB));
    }
    IA = EA;
    IB = EB;
  }
  if (Result == 0)
  {
    Result = (A.size() - IA > 0) ? 1 : ((B.size() - IB > 0) ? -1 : 0);
  }
  if (Result == 0)
  {
    // For deterministic results
    Result = A.compare(B);
  }
  return Result;
}

QList<std::shared_ptr<SessionData>> StoredSessionList::Import(const QList<std::shared_ptr<SessionData>> & From)
{
  // TStoredSessionList::Import
  QList<std::shared_ptr<SessionData>> Result;
  for (const auto & Data : From)
  {
    auto Session = std::make_shared<SessionData>(*Data);
    Session->MakeUniqueIn(*this);
    Add(Session);
    Result.append(Session);
  }
  return Result;
}

std::shared_ptr<SessionData> StoredSessionList::NewSession(const QString & Name, const SessionData & Data)
{
  // TStoredSessionList::NewSession
  auto Session = std::make_shared<SessionData>(Data);
  Session->Name = Name;
  Add(Session);
  return Session;
}

void StoredSessionList::SetDefaultSettings(const SessionData & Value)
{
  // Each site keeps the values it stores (those differing from the current
  // defaults) on top of the new defaults
  auto NewDefaults = std::make_shared<SessionData>(Value);
  NewDefaults->Name = DefaultName;
  NewDefaults->IsWorkspace = false;
  NewDefaults->NoteOnly = false;
  NewDefaults->Link.clear();
  NewDefaults->NameOverride.clear();
  for (auto & Data : FSessions)
  {
    IniFile File;
    IniStorage Storage(&File);
    Storage.OpenSubKey(QStringLiteral("Site"), true);
    Data->Save(Storage, FDefaultSettings.get());
    auto Rebased = std::make_shared<SessionData>(*NewDefaults);
    Rebased->Load(Storage);
    Rebased->Name = Data->Name;
    Rebased->KnownHostsFile = Data->KnownHostsFile;
    // passwords are encrypted with the user and host name, which may now be
    // inherited differently; they always belong to the site itself
    Rebased->Password = Data->Password;
    Rebased->ProxyPassword = Data->ProxyPassword;
    Rebased->TunnelPassword = Data->TunnelPassword;
    Rebased->TunnelPassphrase = Data->TunnelPassphrase;
    *Data = *Rebased;
  }
  FDefaultSettings = NewDefaults;
}

bool StoredSessionList::IsFolder(const QString & Name) const
{
  // TStoredSessionList::IsFolder: first session in the folder is not a workspace entry
  QString Prefix = Name.endsWith(QLatin1Char('/')) ? Name : (Name + QLatin1Char('/'));
  for (const auto & Data : FSessions)
  {
    if (Data->Name.startsWith(Prefix, Qt::CaseInsensitive))
    {
      return !Data->IsWorkspace;
    }
  }
  return false;
}

bool StoredSessionList::IsWorkspace(const QString & Name) const
{
  QString Prefix = Name.endsWith(QLatin1Char('/')) ? Name : (Name + QLatin1Char('/'));
  for (const auto & Data : FSessions)
  {
    if (Data->Name.startsWith(Prefix, Qt::CaseInsensitive))
    {
      return Data->IsWorkspace;
    }
  }
  return false;
}

bool StoredSessionList::CanOpen(const SessionData & Data) const
{
  // TStoredSessionList::CanOpen: resolves workspace links
  const SessionData * Resolved = &Data;
  std::shared_ptr<SessionData> Linked;
  for (int Depth = 0; !Resolved->Link.isEmpty(); Depth++)
  {
    Linked = FindByName(Resolved->Link);
    if ((Linked == nullptr) || (Depth >= 32))
    {
      return false;
    }
    Resolved = Linked.get();
  }
  return Resolved->CanOpen();
}

std::shared_ptr<SessionData> StoredSessionList::FindByName(const QString & Name) const
{
  for (const auto & Data : FSessions)
  {
    if (Data->Name.compare(Name, Qt::CaseInsensitive) == 0)
    {
      return Data;
    }
  }
  return nullptr;
}

void StoredSessionList::Add(std::shared_ptr<SessionData> Data)
{
  Remove(Data->Name);
  FDeleted.removeAll(Data->Name);
  FSessions.append(Data);
  std::sort(FSessions.begin(), FSessions.end(),
    [](const auto & A, const auto & B) { return A->Name.compare(B->Name, Qt::CaseInsensitive) < 0; });
}

void StoredSessionList::Remove(const QString & Name)
{
  for (int I = 0; I < FSessions.size(); I++)
  {
    if (FSessions[I]->Name.compare(Name, Qt::CaseInsensitive) == 0)
    {
      FSessions.removeAt(I);
      FDeleted.append(Name);
      return;
    }
  }
}

void StoredSessionList::Replace(const QStringList & Names, const QList<std::shared_ptr<SessionData>> & Items)
{
  // The fold of Qt::CaseInsensitive, as Remove and Add compare
  QSet<QString> Gone;
  for (const QString & Name : Names)
  {
    Gone.insert(Name.toCaseFolded());
  }
  QSet<QString> Added;
  for (const auto & Data : Items)
  {
    Gone.insert(Data->Name.toCaseFolded());
    Added.insert(Data->Name);
  }
  QList<std::shared_ptr<SessionData>> Kept;
  for (const auto & Data : FSessions)
  {
    if (Gone.contains(Data->Name.toCaseFolded()))
    {
      FDeleted.append(Data->Name);
    }
    else
    {
      Kept.append(Data);
    }
  }
  FDeleted.removeIf([&Added](const QString & Name) { return Added.contains(Name); });
  Kept.append(Items);
  FSessions = Kept;
  std::sort(FSessions.begin(), FSessions.end(),
    [](const auto & A, const auto & B) { return A->Name.compare(B->Name, Qt::CaseInsensitive) < 0; });
}

void StoredSessionList::RenameFolder(const QString & From, const QString & To)
{
  QString Prefix = From + QLatin1Char('/');
  for (const auto & Data : FSessions)
  {
    if (Data->Name.startsWith(Prefix))
    {
      FDeleted.append(Data->Name);
      Data->Name = To + QLatin1Char('/') + Data->Name.mid(Prefix.size());
      FDeleted.removeAll(Data->Name);
    }
  }
  for (QString & F : FFolders)
  {
    if ((F == From) || F.startsWith(Prefix))
    {
      F = To + F.mid(From.size());
    }
  }
}

void StoredSessionList::RemoveFolder(const QString & Folder)
{
  QString Prefix = Folder + QLatin1Char('/');
  for (int I = FSessions.size() - 1; I >= 0; I--)
  {
    if (FSessions[I]->Name.startsWith(Prefix))
    {
      FDeleted.append(FSessions[I]->Name);
      FSessions.removeAt(I);
    }
  }
  FFolders.removeIf([&](const QString & F) { return (F == Folder) || F.startsWith(Prefix); });
}

void StoredSessionList::AddFolder(const QString & Folder)
{
  if (!FFolders.contains(Folder))
  {
    FFolders.append(Folder);
  }
}
