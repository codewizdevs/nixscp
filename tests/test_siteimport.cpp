// Site imports (OpenSSH, PuTTY, FileZilla, INI, known_hosts), PuTTY host key
// conversion, Windows path remapping and the SessionData helpers used by the
// Login dialog (URL parsing, raw settings, Default Settings inheritance)
#include "core/IniStorage.h"
#include "core/Security.h"
#include "core/SessionData.h"
#include "core/SiteImport.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <cstdio>

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); Failures++; } } while (0)

static void WriteFile(const QString & Path, const QByteArray & Data)
{
  QDir().mkpath(QFileInfo(Path).absolutePath());
  QFile F(Path);
  CHECK(F.open(QIODevice::WriteOnly));
  F.write(Data);
}

static QByteArray ReadFile(const QString & Path)
{
  QFile F(Path);
  return F.open(QIODevice::ReadOnly) ? F.readAll() : QByteArray();
}

static int IndexOf(const ImportSessionList & List, const QString & Name)
{
  for (int Index = 0; Index < List.Count(); Index++)
  {
    if (List.Sessions[Index]->Name == Name)
    {
      return Index;
    }
  }
  return -1;
}

static void TestOpenssh(const QString & Home)
{
  WriteFile(Home + QStringLiteral("/.ssh/config"),
    "# comment\n"
    "Host web web-alias\n"
    "  HostName web.example.com\n"
    "  User deploy\n"
    "  Port 2222\n"
    "  IdentityFile ~/.ssh/id_web\n"
    "  User ignored-second-value\n"
    "\n"
    "Host db\n"
    "  HostName=db.internal\n"
    "  ProxyJump admin@bastion:2200\n"
    "  ForwardAgent yes\n"
    "\n"
    "Host bastion\n"
    "  IdentityFile \"/keys/bastion key\"\n"
    "\n"
    "Match host foo\n"
    "  User matched\n"
    "\n"
    "Include conf.d/*.conf\n"
    "\n"
    "Host *.example.com !nope.example.com\n"
    "  Compression yes\n"
    "\n"
    "Host *\n"
    "  User everyone\n");
  WriteFile(Home + QStringLiteral("/.ssh/conf.d/10-extra.conf"),
    "Host included\n"
    "  HostName inc.example.com\n");

  StoredSessionList Stored;
  ImportSessionList List = SiteImport::SelectOpensshSessionsForImport(Stored);
  CHECK(List.Error.isEmpty());
  CHECK(List.Count() == 5); // web, web-alias, db, bastion, included
  int Web = IndexOf(List, QStringLiteral("web"));
  CHECK(Web >= 0);
  if (Web >= 0)
  {
    const SessionData & D = *List.Sessions[Web];
    CHECK(D.HostName == QStringLiteral("web.example.com"));
    CHECK(D.UserName == QStringLiteral("deploy"));
    CHECK(D.PortNumber == 2222);
    CHECK(D.PublicKeyFile == Home + QStringLiteral("/.ssh/id_web"));
    // "Host *.example.com" matches the site name "web"? no: the pattern
    // is matched against the Host alias, as in WinSCP
    CHECK(!D.Compression);
    CHECK(List.Selected[Web]);
  }
  int Db = IndexOf(List, QStringLiteral("db"));
  CHECK(Db >= 0);
  if (Db >= 0)
  {
    const SessionData & D = *List.Sessions[Db];
    CHECK(D.HostName == QStringLiteral("db.internal"));
    CHECK(D.UserName == QStringLiteral("everyone"));
    CHECK(D.AgentFwd);
    CHECK(D.Tunnel);
    CHECK(D.TunnelHostName == QStringLiteral("bastion"));
    CHECK(D.TunnelPortNumber == 2200);
    CHECK(D.TunnelUserName == QStringLiteral("admin"));
    CHECK(D.TunnelPublicKeyFile == QStringLiteral("/keys/bastion key"));
  }
  int Included = IndexOf(List, QStringLiteral("included"));
  CHECK(Included >= 0);
  if (Included >= 0)
  {
    CHECK(List.Sessions[Included]->HostName == QStringLiteral("inc.example.com"));
  }
  CHECK(IndexOf(List, QStringLiteral("*")) < 0);

  // already stored sites are not preselected
  auto Existing = std::make_shared<SessionData>();
  Existing->Name = QStringLiteral("web");
  Existing->HostName = QStringLiteral("x");
  Stored.Add(Existing);
  List = SiteImport::SelectOpensshSessionsForImport(Stored);
  Web = IndexOf(List, QStringLiteral("web"));
  CHECK((Web >= 0) && !List.Selected[Web]);
  // importing makes the names unique
  QList<std::shared_ptr<SessionData>> Imported = Stored.Import({List.Sessions[Web]});
  CHECK(Imported.size() == 1);
  CHECK(Imported.first()->Name == QStringLiteral("web (1)"));
  Imported = Stored.Import({List.Sessions[Web]});
  CHECK(Imported.first()->Name == QStringLiteral("web (2)"));

  // directive parsing
  QString Directive, Value;
  CHECK(SiteImport::ParseOpensshDirective(QStringLiteral("  Port = 22 "), Directive, Value));
  CHECK((Directive == QStringLiteral("Port")) && (Value == QStringLiteral("22")));
  CHECK(!SiteImport::ParseOpensshDirective(QStringLiteral("# Port 22"), Directive, Value));
  QString Tokens = QStringLiteral("\"a b\" c\\ d 'e'");
  CHECK(SiteImport::CutOpensshToken(Tokens) == QStringLiteral("a b"));
  CHECK(SiteImport::CutOpensshToken(Tokens) == QStringLiteral("c d"));
  CHECK(SiteImport::CutOpensshToken(Tokens) == QStringLiteral("e"));

  QFile::remove(Home + QStringLiteral("/.ssh/config"));
  List = SiteImport::SelectOpensshSessionsForImport(Stored);
  CHECK(List.Count() == 0);
  CHECK(List.Error.startsWith(QStringLiteral("OpenSSH config file not found.")));
}

// "Host *" with ProxyJump (and hosts jumping through each other) must not
// recurse without end
static void TestOpensshProxyJumpCycle()
{
  QStringList Lines = {
    QStringLiteral("Host a"), QStringLiteral("  ProxyJump b"),
    QStringLiteral("Host b"), QStringLiteral("  ProxyJump a"),
    QStringLiteral("Host *"), QStringLiteral("  ProxyJump gateway") };
  SessionData Data;
  Data.Name = QStringLiteral("a");
  SiteImport::ImportFromOpenssh(Data, Lines);
  CHECK(Data.Tunnel);
  CHECK(Data.TunnelHostName == QStringLiteral("b"));
  SessionData Other;
  Other.Name = QStringLiteral("other");
  SiteImport::ImportFromOpenssh(Other, Lines);
  CHECK(Other.Tunnel && (Other.TunnelHostName == QStringLiteral("gateway")));
}

static void TestPutty(const QString & Home)
{
  QString Sessions = Home + QStringLiteral("/.putty/sessions");
  WriteFile(Sessions + QStringLiteral("/My%20Server"),
    "HostName=putty.example.com\n"
    "PortNumber=2022\n"
    "UserName=pu\n"
    "Protocol=ssh\n"
    "PublicKeyFile=/home/me/key.ppk\n"
    "Compression=1\n"
    "TerminalType=xterm\n"
    "Font=client:Monospace 12\n");
  WriteFile(Sessions + QStringLiteral("/Telnet%20Box"),
    "HostName=old.example.com\n"
    "PortNumber=23\n"
    "Protocol=telnet\n");
  WriteFile(Sessions + QStringLiteral("/Jump"),
    "HostName=inner.example.com\n"
    "Protocol=ssh\n"
    "ProxyMethod=6\n"
    "ProxyHost=jump.example.com\n"
    "ProxyPort=22\n"
    "ProxyUsername=ju\n");
  WriteFile(Sessions + QStringLiteral("/Default%20Settings"), "HostName=\n");
  WriteFile(Home + QStringLiteral("/.putty/sshhostkeys"),
    "ssh-ed25519@2022:putty.example.com 0x7d13c0aec2fceeeb3a6f07b2f730a56b43febdedb7a926e512b6f33248419950,"
    "0x560fdc920b15181bc5adca45ba158130533d1eb53801cfa9c8c1caf33df11b45\n"
    "rsa2@22:other.example.com 0x10001,0xabcdef\n");

  StoredSessionList Stored;
  ImportSessionList List = SiteImport::SelectPuttySessionsForImport(Stored);
  CHECK(List.Error.isEmpty());
  CHECK(List.Count() == 3);
  int Server = IndexOf(List, QStringLiteral("My Server"));
  CHECK(Server >= 0);
  if (Server >= 0)
  {
    const SessionData & D = *List.Sessions[Server];
    CHECK(D.HostName == QStringLiteral("putty.example.com"));
    CHECK(D.PortNumber == 2022);
    CHECK(D.UserName == QStringLiteral("pu"));
    CHECK(D.PublicKeyFile == QStringLiteral("/home/me/key.ppk"));
    CHECK(D.Compression);
    CHECK(List.Selected[Server]);
    // PuTTY terminal settings are not kept
    IniFile File;
    IniStorage Storage(&File);
    Storage.OpenSubKey(QStringLiteral("S"), true);
    D.Save(Storage, nullptr);
    CHECK(!Storage.ValueExists(QStringLiteral("TerminalType")));
    CHECK(!Storage.ValueExists(QStringLiteral("Font")));
  }
  int Telnet = IndexOf(List, QStringLiteral("Telnet Box"));
  CHECK((Telnet >= 0) && !List.Selected[Telnet]);
  int Jump = IndexOf(List, QStringLiteral("Jump"));
  CHECK(Jump >= 0);
  if (Jump >= 0)
  {
    const SessionData & D = *List.Sessions[Jump];
    CHECK(D.Tunnel);
    CHECK(D.TunnelHostName == QStringLiteral("jump.example.com"));
    CHECK(D.TunnelUserName == QStringLiteral("ju"));
    CHECK(D.ProxyMethod == pmNone);
  }

  // host keys of the imported sites go to known_hosts
  QString KnownHosts = Home + QStringLiteral("/kh/known_hosts");
  QMap<QString, QString> Keys = SiteImport::LoadPuttyHostKeys();
  CHECK(Keys.size() == 2);
  CHECK(SiteImport::ImportHostKeys(Keys, List.SelectedSessions(), KnownHosts) == 1);
  QByteArray Contents = ReadFile(KnownHosts);
  CHECK(Contents == "[putty.example.com]:2022 ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIEUb8T3zysHIqc8BOLUePVMwgRW6RcqtxRsYFQuS3A9W\n");
  CHECK((QFile::permissions(KnownHosts) & (QFileDevice::ReadGroup | QFileDevice::ReadOther)) == 0);
  // not added twice
  CHECK(SiteImport::ImportHostKeys(Keys, List.SelectedSessions(), KnownHosts) == 0);
}

static void TestHostKeyConversion()
{
  CHECK(SiteImport::PuttyHostKeyToKnownHostsLine(QStringLiteral("rsa2@22:rsa.example.com"),
    QStringLiteral("0x10001,0xa75105a74bc03aa267c2e948433bdd66244a97692e67ca08b5af6a1e8d8e92f73d5281993e3d6e307333db4a653a2d0021c2a8497db68798fc262dcd5be09af8b998d4c9bdf15f915e5146dd368c76b04eb6d50cb7d5bedb07c577f0c9712c4bfe06ae812331c38021584d2c237fbfaddc7400a5fa8a3907c7f859dd957d6eab")) ==
    QStringLiteral("rsa.example.com ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAAAgQCnUQWnS8A6omfC6UhDO91mJEqXaS5nygi1r2oejY6S9z1SgZk+PW4wczPbSmU6LQAhwqhJfbaHmPwmLc1b4Jr4uZjUyb3xX5FeUUbdNox2sE621Qy31b7bB8V38MlxLEv+Bq6BIzHDgCFYTSwjf7+t3HQApfqKOQfH+FndlX1uqw=="));
  CHECK(SiteImport::PuttyHostKeyToKnownHostsLine(QStringLiteral("ecdsa-sha2-nistp256@22:10.0.0.1"),
    QStringLiteral("nistp256,0xe3f212c73941b2f0bd1401e844a1e181ce74c13c537f4f13757679d6a446987b,0x4961135541c3f1d494960d6702e9ec81132cfd76ebeb3f8d71681b98da6cad9c")) ==
    QStringLiteral("10.0.0.1 ecdsa-sha2-nistp256 AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBOPyEsc5QbLwvRQB6ESh4YHOdME8U39PE3V2edakRph7SWETVUHD8dSUlg1nAunsgRMs/Xbr6z+NcWgbmNpsrZw="));
  QString Error;
  CHECK(SiteImport::PuttyHostKeyToKnownHostsLine(QStringLiteral("bogus@22:h"), QStringLiteral("0x1"), &Error).isEmpty());
  CHECK(!Error.isEmpty());
}

static void TestFilezilla(const QString & Home)
{
  QString Folder = Home + QStringLiteral("/.config/filezilla");
  WriteFile(Folder + QStringLiteral("/sitemanager.xml"),
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<FileZilla3 version=\"3.66\">\n"
    "  <Servers>\n"
    "    <Server>\n"
    "      <Host>ftp.example.com</Host><Port>21</Port><Protocol>4</Protocol>\n"
    "      <Type>0</Type><User>fz</User><Pass encoding=\"base64\">cGFzc3fDtnJk</Pass>\n"
    "      <Logontype>1</Logontype><PasvMode>MODE_ACTIVE</PasvMode>\n"
    "      <Comments>hello</Comments><LocalDir>/home/me/www</LocalDir>\n"
    "      <RemoteDir>1 0 3 var 3 www</RemoteDir><SyncBrowsing>1</SyncBrowsing>\n"
    "      <Name>Web/FTP</Name>\n"
    "    </Server>\n"
    "    <Folder expanded=\"1\">Customers\n"
    "      <Server>\n"
    "        <Host>sftp.example.com</Host><Port>22</Port><Protocol>1</Protocol>\n"
    "        <Logontype>0</Logontype><Name>Anon</Name>\n"
    "      </Server>\n"
    "    </Folder>\n"
    "  </Servers>\n"
    "</FileZilla3>\n");
  WriteFile(Folder + QStringLiteral("/filezilla.xml"),
    "<?xml version=\"1.0\"?><FileZilla3><Settings>"
    "<Setting name=\"SFTP keyfiles\">/home/me/.ssh/fz_key</Setting>"
    "</Settings></FileZilla3>\n");
  StoredSessionList Stored;
  ImportSessionList List = SiteImport::SelectFilezillaSessionsForImport(Stored);
  CHECK(List.Error.isEmpty());
  CHECK(List.Count() == 2);
  int Ftp = IndexOf(List, QStringLiteral("Web\\FTP"));
  CHECK(Ftp >= 0);
  if (Ftp >= 0)
  {
    const SessionData & D = *List.Sessions[Ftp];
    CHECK(D.HostName == QStringLiteral("ftp.example.com"));
    CHECK(D.FSProtocol == fsFTP);
    CHECK(D.Ftps == ftpsExplicitTls);
    CHECK(D.UserName == QStringLiteral("fz"));
    CHECK(D.Password == QString::fromUtf8("passw\xC3\xB6rd"));
    CHECK(!D.FtpPasvMode);
    CHECK(D.Note == QStringLiteral("hello"));
    CHECK(D.LocalDirectory == QStringLiteral("/home/me/www"));
    CHECK(D.RemoteDirectory == QStringLiteral("/var/www"));
    CHECK(D.SynchronizeBrowsing);
  }
  int Anon = IndexOf(List, QStringLiteral("Customers/Anon"));
  CHECK(Anon >= 0);
  if (Anon >= 0)
  {
    const SessionData & D = *List.Sessions[Anon];
    CHECK(D.FSProtocol == fsSFTP);
    CHECK(D.UserName == QStringLiteral("anonymous"));
    CHECK(D.Password == QStringLiteral("anonymous@example.com"));
    CHECK(D.PublicKeyFile == QStringLiteral("/home/me/.ssh/fz_key"));
  }
  StoredSessionList Empty;
  List = SiteImport::SelectFilezillaSessionsForImport(Empty, Home + QStringLiteral("/nowhere"));
  CHECK(List.Error.startsWith(QStringLiteral("FileZilla site manager file not found")));
}

static void TestIni(const QString & Home)
{
  QString Ini = Home + QStringLiteral("/WinSCP.ini");
  QString Password = QString::fromLatin1(Security::EncryptPassword(QStringLiteral("pw"), QStringLiteral("meini.example.com")));
  WriteFile(Ini, QStringLiteral(
    "[Sessions\\Default%20Settings]\r\n"
    "Timeout=40\r\n"
    "[Sessions\\Office/Ini%20Site]\r\n"
    "HostName=ini.example.com\r\n"
    "UserName=me\r\n"
    "Password=%1\r\n"
    "PublicKeyFile=C:%5CUsers%5Cme%5C.ssh%5Cid_ini.ppk\r\n"
    "LocalDirectory=C:%5CUsers%5Cme%5CDocuments\r\n"
    "[SshHostKeys]\r\n"
    "ssh-ed25519@22:ini.example.com=0x7d13c0aec2fceeeb3a6f07b2f730a56b43febdedb7a926e512b6f33248419950,0x560fdc920b15181bc5adca45ba158130533d1eb53801cfa9c8c1caf33df11b45\r\n").arg(Password).toLatin1());
  StoredSessionList Stored;
  ImportSessionList List = SiteImport::SelectSessionsForImport(Stored, QString());
  CHECK(List.Error == QStringLiteral("Browse for INI file to import sites from."));
  List = SiteImport::SelectSessionsForImport(Stored, Ini);
  CHECK(List.Error.isEmpty());
  CHECK(List.Count() == 1);
  if (List.Count() == 1)
  {
    SessionData & D = *List.Sessions.first();
    CHECK(D.Name == QStringLiteral("Office/Ini Site"));
    CHECK(D.Password == QStringLiteral("pw"));
    CHECK(D.Timeout == 40);
    CHECK(List.Selected.first());
    CHECK(SiteImport::HasWindowsPaths(D));
    // Windows paths: the profile maps to the home folder
    QDir().mkpath(Home + QStringLiteral("/Documents"));
    WriteFile(Home + QStringLiteral("/.ssh/id_ini"), "key");
    CHECK(SiteImport::RemapWindowsPaths(D));
    CHECK(D.LocalDirectory == Home + QStringLiteral("/Documents"));
    CHECK(D.PublicKeyFile == Home + QStringLiteral("/.ssh/id_ini"));
    CHECK(!SiteImport::HasWindowsPaths(D));
  }
  QMap<QString, QString> Keys = SiteImport::LoadIniHostKeys(Ini);
  CHECK(Keys.size() == 1);
  QString KnownHosts = Home + QStringLiteral("/kh2/known_hosts");
  CHECK(SiteImport::ImportHostKeys(Keys, List.SelectedSessions(), KnownHosts) == 1);
  CHECK(ReadFile(KnownHosts).startsWith("ini.example.com ssh-ed25519 AAAAC3NzaC1lZDI1NTE5"));
  CHECK(SiteImport::RemapWindowsPath(QStringLiteral("D:\\Data\\nothing"), true).isEmpty());
  CHECK(SiteImport::RemapWindowsPath(QStringLiteral("/already/linux"), true) == QStringLiteral("/already/linux"));
}

static void TestKnownHosts(const QString & Home)
{
  QString Target = Home + QStringLiteral("/kh3/known_hosts");
  WriteFile(Target, "known.example.com ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIEUb8T3zysHIqc8BOLUePVMwgRW6RcqtxRsYFQuS3A9W");
  QStringList Lines = {
    QStringLiteral("known.example.com ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIEUb8T3zysHIqc8BOLUePVMwgRW6RcqtxRsYFQuS3A9W"),
    QStringLiteral("[new.example.com]:2222,10.1.1.1 ecdsa-sha2-nistp256 AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBOPyEsc5QbLwvRQB6ESh4YHOdME8U39PE3V2edakRph7SWETVUHD8dSUlg1nAunsgRMs/Xbr6z+NcWgbmNpsrZw="),
    QStringLiteral("|1|hashed|host ssh-ed25519 AAAA"),
    QStringLiteral("# comment") };
  StoredSessionList Stored;
  ImportSessionList List = SiteImport::SelectKnownHostsSessionsForImport(Stored, Lines, Target);
  CHECK(List.Count() == 2);
  int Known = IndexOf(List, QStringLiteral("known.example.com"));
  int New = IndexOf(List, QStringLiteral("new.example.com:2222"));
  CHECK((Known >= 0) && !List.Selected[Known]);
  CHECK((New >= 0) && List.Selected[New]);
  if (New >= 0)
  {
    CHECK(List.Sessions[New]->PortNumber == 2222);
  }
  CHECK(SiteImport::ImportSelectedKnownHosts(List, Target) == 1);
  QByteArray Contents = ReadFile(Target);
  CHECK(Contents.count('\n') == 2);
  CHECK(Contents.contains("\n[new.example.com]:2222,10.1.1.1 ecdsa-sha2-nistp256 "));
  CHECK(SiteImport::ImportSelectedKnownHosts(List, Target) == 0);
  List = SiteImport::SelectKnownHostsSessionsForImport(Stored, {QStringLiteral("garbage")}, Target);
  CHECK(List.Error == QStringLiteral("No host keys found in known_hosts."));
}

static void TestSessionData()
{
  // URL / user@host typed into the Login dialog host box
  SessionData D;
  D.PortNumber = 2222;
  D.Password = QStringLiteral("old");
  int ParsedInfo = 0;
  CHECK(D.ParseUrl(QStringLiteral("me@host.example.com"), ParsedInfo));
  CHECK((ParsedInfo & SessionData::ParsedProtocolDefined) == 0);
  CHECK(D.HostName == QStringLiteral("host.example.com"));
  CHECK(D.UserName == QStringLiteral("me"));
  CHECK(D.Password.isEmpty());
  CHECK(D.PortNumber == 2222);
  CHECK(D.FSProtocol == fsSFTP);
  SessionData F;
  CHECK(F.ParseUrl(QStringLiteral("ftpes://u%40x:p%3Aw@ftp.example.com/pub/dir"), ParsedInfo));
  CHECK((ParsedInfo & SessionData::ParsedProtocolDefined) != 0);
  CHECK(F.FSProtocol == fsFTP);
  CHECK(F.Ftps == ftpsExplicitTls);
  CHECK(F.PortNumber == 21);
  CHECK(F.UserName == QStringLiteral("u@x"));
  CHECK(F.Password == QStringLiteral("p:w"));
  CHECK(F.RemoteDirectory == QStringLiteral("/pub/dir"));
  SessionData V6;
  CHECK(V6.ParseUrl(QStringLiteral("scp://[fe80::1]:2200/"), ParsedInfo));
  CHECK(V6.HostName == QStringLiteral("fe80::1"));
  CHECK(V6.PortNumber == 2200);
  CHECK(V6.FSProtocol == fsSCPonly);
  CHECK(SessionData::IsIPv6Literal(QStringLiteral("fe80::1")));
  CHECK(!SessionData::IsIPv6Literal(QStringLiteral("host:22")));
  SessionData Unsafe;
  CHECK(Unsafe.ParseUrl(QStringLiteral("sftp://u;x-Shell=%2Fbin%2Fevil@h/"), ParsedInfo));
  CHECK((ParsedInfo & SessionData::ParsedUnsafeSettings) != 0);
  CHECK(Unsafe.Shell == QStringLiteral("/bin/evil"));
  SessionData Safe;
  CHECK(Safe.ParseUrl(QStringLiteral("sftp://u;x-Compression=1;fingerprint=ssh-ed25519%20255%20abc@h/"), ParsedInfo));
  CHECK((ParsedInfo & SessionData::ParsedUnsafeSettings) == 0);
  CHECK(Safe.Compression);
  CHECK(Safe.HostKey == QStringLiteral("ssh-ed25519 255 abc"));

  // environment variables
  qputenv("NIXSCP_TEST_VAR", "value");
  CHECK(SessionData::ExpandEnvironmentVariables(QStringLiteral("$NIXSCP_TEST_VAR/${NIXSCP_TEST_VAR}/%NIXSCP_TEST_VAR%")) ==
        QStringLiteral("value/value/value"));
  CHECK(SessionData::ExpandEnvironmentVariables(QStringLiteral("$NIXSCP_UNDEFINED_VAR 100%")) == QStringLiteral("$NIXSCP_UNDEFINED_VAR 100%"));
  CHECK(SessionData::ExpandEnvironmentVariables(QStringLiteral("~/x")) == QDir::homePath() + QStringLiteral("/x"));
  CHECK(SessionData::ExpandEnvironmentVariables(QStringLiteral("%USERPROFILE%")) == QDir::homePath());
  SessionData E;
  E.HostName = QStringLiteral("${NIXSCP_TEST_VAR}.example.com");
  E.PublicKeyFile = QStringLiteral("~/.ssh/id");
  E.ExpandEnvironmentVariables();
  CHECK(E.HostName == QStringLiteral("value.example.com"));
  CHECK(E.PublicKeyFile == QDir::homePath() + QStringLiteral("/.ssh/id"));

  // raw settings (Edit Raw Settings dialog)
  SessionData R;
  R.Compression = true;
  R.ProxyTelnetCommand = QStringLiteral("connect %host %port");
  R.Note = QStringLiteral("a b%c");
  SessionData Factory;
  QStringList Options = R.SaveToOptions(&Factory);
  CHECK(Options.contains(QStringLiteral("Compression=1")));
  CHECK(Options.contains(QStringLiteral("ProxyTelnetCommand=connect %host %port")));
  CHECK(Options.contains(QStringLiteral("Note=a b%c")));
  CHECK(!Options.contains(QStringLiteral("Timeout=15")));
  SessionData R2;
  CHECK(R2.ApplyRawSettings(Options)); // proxy command is unsafe
  CHECK(R2.Compression);
  CHECK(R2.Note == QStringLiteral("a b%c"));
  CHECK(R2.ProxyTelnetCommand == QStringLiteral("connect %host %port"));
  SessionData R3;
  CHECK(!R3.ApplyRawSettings({QStringLiteral("Timeout=99"), QStringLiteral("UnknownKey=x")}));
  CHECK(R3.Timeout == 99);
  CHECK(R3.SaveToOptions(&Factory).contains(QStringLiteral("UnknownKey=x")));

  // unique names
  StoredSessionList List;
  auto A = std::make_shared<SessionData>();
  A->Name = QStringLiteral("site");
  List.Add(A);
  SessionData B;
  B.Name = QStringLiteral("site");
  B.MakeUniqueIn(List);
  CHECK(B.Name == QStringLiteral("site (1)"));

  // natural order
  CHECK(StoredSessionList::CompareLogicalText(QStringLiteral("site2"), QStringLiteral("site10")) < 0);
  CHECK(StoredSessionList::CompareLogicalText(QStringLiteral("Alpha"), QStringLiteral("beta")) < 0);

  // Set Defaults: sites keep their own values and inherit the rest
  StoredSessionList Sites;
  auto S1 = std::make_shared<SessionData>(Sites.DefaultSettings());
  S1->Name = QStringLiteral("s1");
  S1->HostName = QStringLiteral("h1");
  S1->Timeout = 50;          // own value
  S1->Password = QStringLiteral("p1");
  Sites.Add(S1);
  SessionData NewDefaults = Sites.DefaultSettings();
  NewDefaults.Timeout = 25;
  NewDefaults.Compression = true;
  NewDefaults.UserName = QStringLiteral("defuser");
  Sites.SetDefaultSettings(NewDefaults);
  CHECK(Sites.DefaultSettings().Compression);
  auto S1After = Sites.FindByName(QStringLiteral("s1"));
  CHECK(S1After != nullptr);
  if (S1After)
  {
    CHECK(S1After->Timeout == 50);
    CHECK(S1After->Compression);
    CHECK(S1After->UserName == QStringLiteral("defuser"));
    CHECK(S1After->HostName == QStringLiteral("h1"));
    CHECK(S1After->Password == QStringLiteral("p1"));
  }
  // and they are stored that way
  IniFile Out;
  Sites.Save(Out);
  CHECK(Out.ReadRaw(QStringLiteral("Sessions\\s1"), QStringLiteral("Timeout")) == QStringLiteral("50"));
  CHECK(!Out.Keys(QStringLiteral("Sessions\\s1")).contains(QStringLiteral("Compression")));
  CHECK(Out.ReadRaw(QStringLiteral("Sessions\\Default%20Settings"), QStringLiteral("Compression")) == QStringLiteral("1"));

  // info tip
  SessionData T;
  T.HostName = QStringLiteral("h");
  T.UserName = QStringLiteral("u");
  CHECK(T.InfoTip() == QStringLiteral("Host: h\nUsername: u\nPrivate key file: No\nTransfer protocol: SFTP (SCP)"));
  T.FSProtocol = fsFTP;
  CHECK(T.InfoTip() == QStringLiteral("Host: h\nUsername: u\nTransfer protocol: FTP"));

  // colors (BGR TColor)
  SessionData C;
  C.Color = SessionData::RgbToColor(0x112233);
  CHECK(C.Color == 0x332211);
  CHECK(C.ColorRgb() == 0x112233u);
  CHECK(SessionData::RgbToColor(0) == 0x010101);

  // DisablePasswordStoring policy
  {
    SessionData::DisablePasswordStoring = true;
    IniFile File;
    IniStorage Storage(&File);
    Storage.OpenSubKey(QStringLiteral("S"), true);
    SessionData P;
    P.HostName = QStringLiteral("h");
    P.Password = QStringLiteral("p");
    P.TunnelPassword = QStringLiteral("t");
    P.Save(Storage, nullptr);
    CHECK(!Storage.ValueExists(QStringLiteral("Password")));
    CHECK(!Storage.ValueExists(QStringLiteral("TunnelPassword")));
    Storage.WriteStringRaw(QStringLiteral("Password"), QString::fromLatin1(Security::EncryptPassword(QStringLiteral("p"), QStringLiteral("h"))));
    SessionData L;
    L.Load(Storage);
    CHECK(L.Password.isEmpty());
    SessionData::DisablePasswordStoring = false;
    SessionData L2;
    L2.Load(Storage);
    CHECK(L2.Password == QStringLiteral("p"));
  }

  CHECK(SessionData::IsValidName(QStringLiteral("a b")));
  CHECK(!SessionData::IsValidName(QStringLiteral("a/b")));
  CHECK(SessionData::MakeValidName(QStringLiteral("a/b")) == QStringLiteral("a\\b"));
}

int main()
{
  QTemporaryDir Dir;
  QString Home = Dir.path();
  qputenv("HOME", Home.toLocal8Bit());
  qputenv("XDG_CONFIG_HOME", (Home + QStringLiteral("/.config")).toLocal8Bit());
  qunsetenv("PUTTYDIR");
  TestOpenssh(Home);
  TestOpensshProxyJumpCycle();
  TestPutty(Home);
  TestHostKeyConversion();
  TestFilezilla(Home);
  TestIni(Home);
  TestKnownHosts(Home);
  TestSessionData();
  if (Failures == 0)
  {
    std::printf("test_siteimport: all tests passed\n");
  }
  return (Failures == 0) ? 0 : 1;
}
