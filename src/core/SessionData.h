// Port of WinSCP source/core/SessionData.{h,cpp}
// Field names, defaults and WinSCP.ini key names follow TSessionData, so sites
// can be copied between WinSCP.ini and nixscp.ini in both directions.
#pragma once

#include <QString>
#include <QList>
#include <QMap>
#include <memory>

class IniStorage;
class IniFile;
class StoredSessionList;

// Values match WinSCP's TFSProtocol so WinSCP.ini files stay compatible
enum TFSProtocol { fsSCPonly = 0, fsSFTP = 1, fsSFTPonly = 2, fsFTP = 5, fsWebDAV = 6, fsS3 = 7 };
enum TFtps { ftpsNone = 0, ftpsImplicit = 1, ftpsExplicitSsl = 2, ftpsExplicitTls = 3 };
// core/Configuration.h; matches PuTTY FORCE_ON, FORCE_OFF, AUTO
enum TAutoSwitch { asOn = 0, asOff = 1, asAuto = 2 };
// pmCmd = "Local" proxy (command whose stdin/stdout is the connection)
enum TProxyMethod { pmNone = 0, pmSocks4 = 1, pmSocks5 = 2, pmHTTP = 3, pmTelnet = 4, pmCmd = 5 };
// SSH keepalives: ptNullPacket = SSH_MSG_IGNORE, ptDummyCommand = executing dummy protocol command
enum TPingType { ptOff = 0, ptNullPacket = 1, ptDummyCommand = 2 };
enum TFtpPingType { fptOff = 0, fptDummyCommand0 = 1, fptDummyCommand = 2, fptDirectoryListing = 3 };
enum TAddressFamily { afAuto = 0, afIPv4 = 1, afIPv6 = 2 };
// core/FileBuffer.h
enum TEOLType { eolLF = 0, eolCRLF = 1, eolCR = 2 };
// core/Common.h; Linux has no "Windows DST bug", dstmWin/dstmUnix are stored only
enum TDSTMode { dstmWin = 0, dstmUnix = 1, dstmKeep = 2 };
enum TTlsVersion { tls10 = 10, tls11 = 11, tls12 = 12, tls13 = 13, tlsDefaultMin = tls12, tlsMax = tls13 };
enum TS3UrlStyle { s3usVirtualHost = 0, s3usPath = 1 };
// Names have to match PuTTY registry entries "Bug<name>"
enum TSshBug { sbHMAC2, sbDeriveKey2, sbRSAPad2, sbPKSessID2, sbRekey2, sbMaxPkt2, sbIgnore2, sbOldGex2, sbWinAdj, sbChanReq, sbCount };
enum TSftpBug { sbSymlink, sbSignedTS, sftpbCount };

constexpr int SshPortNumber = 22;
constexpr int FtpPortNumber = 21;
constexpr int FtpsImplicitPortNumber = 990;
constexpr int HTTPPortNumber = 80;
constexpr int HTTPSPortNumber = 443;
constexpr int ProxyPortNumber = 80;
constexpr int SFTPMaxVersionAuto = -1;
constexpr int DefaultSendBuf = 262144;

class SessionData
{
public:
  SessionData();

  void Default();
  void Load(IniStorage & Storage);
  // Default: values equal to it are deleted instead of written (as WinSCP
  // saves sites relative to "Default Settings"); nullptr writes everything.
  void Save(IniStorage & Storage, const SessionData * Default = nullptr) const;

  // Site name, may contain '/' for folders (as in WinSCP)
  QString Name;

  // ---- Session (Login dialog) ----
  QString HostName;                 // HostName (may be stored as user@host)
  int PortNumber;                   // PortNumber
  QString UserName;                 // UserName
  QString Password;                 // Password (encrypted, key UserName+HostName) / PasswordPlain
  TFSProtocol FSProtocol;           // FSProtocol
  TFtps Ftps;                       // Ftps
  bool IsWorkspace;                 // IsWorkspace
  QString Link;                     // Link: workspace entry referring to a stored site (core/Workspaces.h)
  QString NameOverride;             // NameOverride: name of an ad-hoc session saved in a workspace
  int Color;                        // Color (BGR TColor, 0 = none)
  QString Note;                     // Note ("Note" page)
  // NixSCP Agent Access (core/AgentAccess.h): AgentAccess::TMode, TPermission bits
  int AgentAccess;                  // AgentAccess
  int AgentPermissions;             // AgentPermissions
  QString AgentName;                // AgentName: the name agents use (empty: the site name)
  QString AgentAllow;               // AgentAllow: allowed command patterns, one per line (empty: all)
  QString AgentDeny;                // AgentDeny: denied command patterns, one per line
  bool AgentRecord;                 // AgentRecord: keep transcripts of agent requests
  QString PortForwards;             // PortForwards: saved local port forwards ("name|local port|host|port" per line)
  bool Special;                     // Special (read only; never written, preserved raw): cannot be deleted/renamed

  // ---- Environment page ----
  TEOLType EOLType;                 // EOLType: end-of-line of the server (SCP/text mode)
  TAutoSwitch NotUtf;               // "Utf" (int = NotUtf; on/off strings reversed): asOff = UTF-8 filenames on
  TDSTMode DSTMode;                 // ConsiderDST: daylight saving time (dstmUnix default)
  double TimeDifference;            // TimeDifference: timezone offset in days (TDateTime units), SCP/FTP
  bool TimeDifferenceAuto;          // TimeDifferenceAuto: FTP only, detect automatically
  bool TrimVMSVersions;             // TrimVMSVersions (n/a, stored)
  bool VMSAllRevisions;             // VMSAllRevisions (n/a, stored)

  // ---- Environment > Directories ----
  QString RemoteDirectory;          // RemoteDirectory
  QString LocalDirectory;           // LocalDirectory
  QString OtherLocalDirectory;      // OtherLocalDirectory
  bool SynchronizeBrowsing;         // SynchronizeBrowsing
  bool UpdateDirectories;           // UpdateDirectories: remember last used directory
  bool CacheDirectories;            // CacheDirectories: cache visited remote directories
  bool CacheDirectoryChanges;       // CacheDirectoryChanges
  bool PreserveDirectoryChanges;    // PreserveDirectoryChanges: permanent cache
  bool ResolveSymlinks;             // ResolveSymlinks
  bool FollowDirectorySymlinks;     // FollowDirectorySymlinks

  // ---- Environment > Recycle bin ----
  bool DeleteToRecycleBin;          // DeleteToRecycleBin (remote: move to RecycleBinPath)
  bool OverwrittenToRecycleBin;     // OverwrittenToRecycleBin
  QString RecycleBinPath;           // RecycleBinPath (remote directory)

  // ---- Environment > SFTP (applied by SftpFileSystem) ----
  QString SftpServer;               // SftpServer: empty = default subsystem, else server binary path
  int SFTPMaxVersion;               // SFTPMaxVersion: -1 auto, else 0..6
  int SFTPMaxPacketSize;            // SFTPMaxPacketSize (0 = auto)
  int SFTPDownloadQueue;            // SFTPDownloadQueue
  int SFTPUploadQueue;              // SFTPUploadQueue
  int SFTPListingQueue;             // SFTPListingQueue
  TAutoSwitch SFTPRealPath;         // SFTPRealPath: canonicalize paths on the server
  bool UsePosixRename;              // UsePosixRename (posix-rename@openssh.com)
  TAutoSwitch SFTPBug[sftpbCount];  // SFTPSymlinkBug (reversed symlink args), SFTPSignedTSBug

  // ---- Environment > Shell / SCP (applied by the SCP file system) ----
  QString Shell;                    // Shell: empty = default
  QString ReturnVar;                // ReturnVar: empty = autodetect
  bool ExitCode1IsError;            // ExitCode1IsError
  TAutoSwitch LookupUserGroups;     // LookupUserGroups2
  bool ClearAliases;                // ClearAliases
  bool UnsetNationalVars;           // UnsetNationalVars
  QString ListingCommand;           // ListingCommand ("ls -la")
  bool IgnoreLsWarnings;            // IgnoreLsWarnings
  TAutoSwitch SCPLsFullTime;        // SCPLsFullTime
  bool Scp1Compatibility;           // Scp1Compatibility
  QString PostLoginCommands;        // PostLoginCommands

  // ---- Environment > FTP (applied by the FTP file system) ----
  bool FtpPasvMode;                 // FtpPasvMode (Connection page)
  TAutoSwitch FtpForcePasvIp;       // FtpForcePasvIp2
  TAutoSwitch FtpUseMlsd;           // FtpUseMlsd
  QString FtpAccount;               // FtpAccount
  TFtpPingType FtpPingType;         // FtpPingType (keepalives for FTP)
  int FtpPingInterval;              // FtpPingInterval (seconds)
  TAutoSwitch FtpTransferActiveImmediately; // FtpTransferActiveImmediately2
  TAutoSwitch FtpListAll;           // FtpListAll ("LIST -a")
  TAutoSwitch FtpHost;              // FtpHost (HOST command)
  TAutoSwitch FtpWorkFromCwd;       // FtpWorkFromCwd
  bool FtpAnyCodeForPwd;            // FtpAnyCodeForPwd
  int FtpProxyLogonType;            // FtpProxyLogonType (0 = none)
  bool SslSessionReuse;             // SslSessionReuse
  QString TlsCertificateFile;       // TlsCertificateFile (client certificate)
  TTlsVersion MinTlsVersion;        // MinTlsVersion
  TTlsVersion MaxTlsVersion;        // MaxTlsVersion
  TAutoSwitch CompleteTlsShutdown;  // CompleteTlsShutdown

  // ---- Environment > S3 / WebDAV (stored) ----
  QString S3DefaultRegion;          // S3DefaultRegion
  QString S3SessionToken;           // S3SessionToken
  QString S3RoleArn;                // S3RoleArn
  QString S3RoleSessionName;        // S3RoleSessionName
  QString S3Profile;                // S3Profile
  TS3UrlStyle S3UrlStyle;           // S3UrlStyle
  TAutoSwitch S3MaxKeys;            // S3MaxKeys
  bool S3CredentialsEnv;            // S3CredentialsEnv
  bool S3RequesterPays;             // S3RequesterPays
  bool WebDavLiberalEscaping;       // WebDavLiberalEscaping
  bool WebDavAuthLegacy;            // WebDavAuthLegacy

  // ---- Connection page ----
  int Timeout;                      // Timeout (seconds)
  TPingType PingType;               // PingType (SSH keepalives)
  int PingInterval;                 // seconds; stored as PingInterval (minutes) + PingIntervalSecs
  TAddressFamily AddressFamily;     // AddressFamily
  bool TcpNoDelay;                  // TcpNoDelay
  int SendBuf;                      // SendBuf
  QString SourceAddress;            // SourceAddress

  // ---- Connection > Proxy (applied by SecureShell / FTP) ----
  TProxyMethod ProxyMethod;         // ProxyMethod (int or none/socks4/socks5/http/telnet/cmd)
  QString ProxyHost;                // ProxyHost ("proxy")
  int ProxyPort;                    // ProxyPort (80)
  QString ProxyUsername;            // ProxyUsername
  QString ProxyPassword;            // ProxyPasswordEnc (key ProxyUsername+ProxyHost) / ProxyPassword (plain)
  QString ProxyTelnetCommand;       // ProxyTelnetCommand when ProxyMethod != pmCmd ("connect %host %port\n")
  QString ProxyLocalCommand;        // ProxyTelnetCommand when ProxyMethod == pmCmd
  TAutoSwitch ProxyDNS;             // ProxyDNS (stored as (X+2)%3): DNS name lookup at proxy end
  bool ProxyLocalhost;              // ProxyLocalhost: proxy also localhost connections

  // ---- Connection > Tunnel (applied by SecureShell) ----
  bool Tunnel;                      // Tunnel: connect through SSH tunnel
  QString TunnelHostName;           // TunnelHostName
  int TunnelPortNumber;             // TunnelPortNumber (22)
  QString TunnelUserName;           // TunnelUserName
  QString TunnelPassword;           // TunnelPassword (key TunnelUserName+TunnelHostName) / TunnelPasswordPlain
  QString TunnelPublicKeyFile;      // TunnelPublicKeyFile
  int TunnelLocalPortNumber;        // TunnelLocalPortNumber (0 = autoselect)
  QString TunnelHostKey;            // TunnelHostKey (fingerprint(s) accepted)

  // ---- SSH page (applied by SecureShell) ----
  bool Compression;                 // Compression
  bool Ssh2DES;                     // Ssh2DES
  QString CipherList;               // Cipher: WinSCP names, see CipherNames()
  QString KexList;                  // KEX: WinSCP names, see KexNames()
  QString HostKeyList;              // HostKey: WinSCP names, see HostKeyNames()
  QString RekeyData;                // RekeyBytes ("1G"; 0 = off; K/M/G suffix)
  int RekeyTime;                    // RekeyTime (minutes, 0 = off)
  QString HostKey;                  // SshHostKey: expected host key fingerprint(s)
  bool SshSimple;                   // SshSimple

  // ---- SSH > Authentication ----
  bool SshNoUserAuth;               // SshNoUserAuth: bypass authentication entirely
  bool TryAgent;                    // TryAgent
  bool AuthKI;                      // AuthKI
  bool AuthKIPassword;              // AuthKIPassword
  bool AgentFwd;                    // AgentFwd
  bool AuthGSSAPI;                  // AuthGSSAPI
  bool AuthGSSAPIKEX;               // AuthGSSAPIKEX
  bool GSSAPIFwdTGT;                // GSSAPIFwdTGT
  bool ChangeUsername;              // ChangeUsername
  QString LogicalHostName;          // LogicalHostName
  QString PublicKeyFile;            // PublicKeyFile (private key file)
  QString DetachedCertificate;      // DetachedCertificate

  // ---- SSH > Bugs ----
  TAutoSwitch Bug[sbCount];         // Bug<name>, stored as 2-X (PuTTY format)

  // Runtime only (not persisted): overrides libssh's default ~/.ssh/known_hosts.
  // libssh resolves ~ via getpwuid(), not $HOME, so tests need this to stay
  // away from the user's real file. Empty = libssh default.
  QString KnownHostsFile;

  // Algorithm names as used in CipherList/KexList/HostKeyList, in WinSCP's
  // enum order; "WARN" is the "-- warn below here --" separator.
  static const QStringList & CipherNames();
  static const QStringList & KexNames();
  static const QStringList & HostKeyNames();
  static QString DefaultCipherList();
  static QString DefaultKexList();
  static QString DefaultHostKeyList();
  // Parses a list (unknown names dropped, duplicates removed) and adds missing
  // algorithms from Defaults, like TSessionData::SetAlgoList
  static QStringList NormalizeAlgoList(const QString & List, const QString & Defaults);
  QStringList Ciphers() const { return NormalizeAlgoList(CipherList, DefaultCipherList()); }
  QStringList Kexes() const { return NormalizeAlgoList(KexList, DefaultKexList()); }
  QStringList HostKeys() const { return NormalizeAlgoList(HostKeyList, DefaultHostKeyList()); }
  // RekeyBytes parsed to bytes (0 = never)
  qint64 RekeyBytes() const;
  QString ProxyCommand() const { return (ProxyMethod == pmCmd) ? ProxyLocalCommand : ProxyTelnetCommand; }
  static const char * const SshBugNames[sbCount];

  bool CanLogin() const { return !HostName.isEmpty(); }
  QString FolderName() const;    // "folder/sub" part of Name
  QString LocalName() const;     // last part of Name
  QString SessionName() const;   // Name, or user@host when unnamed
  QString DefaultSessionName() const;
  QString ProtocolStr() const;
  QString SessionUrl() const;
  bool IsSsh() const { return (FSProtocol == fsSCPonly) || (FSProtocol == fsSFTP) || (FSProtocol == fsSFTPonly); }
  int DefaultPort() const { return DefaultPort(FSProtocol, Ftps); }

  static int DefaultPort(TFSProtocol Protocol, TFtps Ftps);

  // Configuration policy DisablePasswordStoring (set by Configuration::Load):
  // session and tunnel passwords are neither loaded from nor saved to sites
  static bool DisablePasswordStoring;

  // TSessionData::HasPassword / HasAnySessionPassword
  bool HasPassword() const { return !Password.isEmpty(); }
  bool HasAnySessionPassword() const { return !Password.isEmpty() || !TunnelPassword.isEmpty(); }
  // TSessionData::GetInfoTip (SESSION_INFO_TIP2 / SESSION_INFO_TIP_NO_SSH)
  QString InfoTip() const;
  // TNamedObject::MakeUniqueIn: appends " (2)", " (3)"... while List has a site of the name
  void MakeUniqueIn(const StoredSessionList & List);
  // Color is a BGR TColor; as 0xRRGGBB (meaningful when Color != 0)
  unsigned int ColorRgb() const { return ((Color & 0xFF) << 16) | (Color & 0xFF00) | ((Color >> 16) & 0xFF); }
  static int RgbToColor(unsigned int Rgb);
  // "Protocol" of imported PuTTY sessions (TSessionData::GetNormalizedPuttyProtocol)
  QString NormalizedPuttyProtocol() const;
  // Drops settings not understood by NixSCP (used for sessions imported from
  // other programs, so that e.g. PuTTY terminal settings do not end up in nixscp.ini)
  void DiscardUnknownSettings() { FRawValues.clear(); }

  // TSessionData::ValidateName / MakeValidName / ComposePath / ExtractFolderName
  static bool IsValidName(const QString & Name, QString * Error = nullptr);
  static QString MakeValidName(const QString & Name);
  static QString ComposePath(const QString & Folder, const QString & Name);

  // ExpandEnvironmentVariables (TSessionData::Get*Expanded). Linux adaptation
  // of ExpandEnvironmentStrings: $VAR, ${VAR}, %VAR% (as in imported
  // WinSCP.ini files) and a leading ~ (home folder). Unknown variables are
  // kept as they are.
  static QString ExpandEnvironmentVariables(const QString & Str);
  QString HostNameExpanded() const { return ExpandEnvironmentVariables(HostName); }
  QString UserNameExpanded() const { return ExpandEnvironmentVariables(UserName); }
  QString LocalDirectoryExpanded() const { return ExpandEnvironmentVariables(LocalDirectory); }
  // Expands host/user names and all local file/directory fields in place
  // (session about to be opened; stored sites keep the unexpanded values)
  void ExpandEnvironmentVariables();

  // TSessionData::ParseUrl as used by the Login dialog (pufPreferProtocol,
  // no stored site lookup): [protocol://][user[:password][;fingerprint=..][;x-name=value]@]host[:port][/path]
  // Protocol, port and encryption are only changed when the URL defines them.
  enum { ParsedProtocolDefined = 0x01, ParsedDefaultsOnly = 0x02, ParsedUnsafeSettings = 0x04 };
  bool ParseUrl(const QString & Url, int & ParsedInfo);
  static bool IsIPv6Literal(const QString & HostName);

  // TSessionData::SaveToOptions: settings as "Key=Value" lines (WinSCP.ini
  // key names, unescaped values), only those differing from Default
  // (nullptr = all). Used by the "Raw Site Settings" dialog and URL code.
  QStringList SaveToOptions(const SessionData * Default) const;
  // TSessionData::ApplyRawSettings: applies "Key=Value" lines; returns true
  // when a potentially unsafe setting (shell, commands, recycle bin, SFTP
  // server, proxy command) changed (UNSAFE_SESSION confirmation)
  bool ApplyRawSettings(const QStringList & Settings);

private:
  // All raw key/values as loaded, so settings that NixSCP does not
  // understand yet survive a load/save round trip.
  QMap<QString, QString> FRawValues;
};

class StoredSessionList
{
public:
  StoredSessionList();

  void Load(IniFile & File);
  void Save(IniFile & File) const;
  // Imports sites from a WinSCP.ini file; returns number of imported sites
  int ImportFromWinSCPIni(const QString & FileName, QString * Error);
  // TStoredSessionList::Import: adds copies of the sites, made unique by
  // name (MakeUniqueIn); returns the added sites
  QList<std::shared_ptr<SessionData>> Import(const QList<std::shared_ptr<SessionData>> & From);
  // TStoredSessionList::NewSession: stores a copy of Data as Name,
  // replacing a site of the same name (case insensitive)
  std::shared_ptr<SessionData> NewSession(const QString & Name, const SessionData & Data);
  // TStoredSessionList::SetDefaultSettings. As sites are stored relative to
  // "Default Settings", sites keep only the values they override and
  // inherit the rest from the new defaults (as after reloading WinSCP.ini).
  void SetDefaultSettings(const SessionData & Value);
  // TStoredSessionList::IsFolder / IsWorkspace / CanOpen
  bool IsFolder(const QString & Name) const;
  bool IsWorkspace(const QString & Name) const;
  bool CanOpen(const SessionData & Data) const;
  // CompareLogicalText (natural order, case insensitive, deterministic)
  static int CompareLogicalText(const QString & A, const QString & B);

  const QList<std::shared_ptr<SessionData>> & Sessions() const { return FSessions; }
  // Empty folders (WinSCP persists them as sites without host via folder entries)
  const QStringList & Folders() const { return FFolders; }
  std::shared_ptr<SessionData> FindByName(const QString & Name) const;
  void Add(std::shared_ptr<SessionData> Data);
  void Remove(const QString & Name);
  void RenameFolder(const QString & From, const QString & To);
  void RemoveFolder(const QString & Folder);
  void AddFolder(const QString & Folder);
  SessionData & DefaultSettings() { return *FDefaultSettings; }

  static QString DefaultSettingsName();

private:
  QList<std::shared_ptr<SessionData>> FSessions;
  QStringList FFolders;
  std::shared_ptr<SessionData> FDefaultSettings;
  QStringList FDeleted;
};
