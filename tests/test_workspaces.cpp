// Workspace tests (core/Workspaces.cpp): WinSCP.ini format of workspaces
// (TStoredSessionList::SaveWorkspaceData/NewWorkspace/GetFolderOrWorkspace)
#include "core/Configuration.h"
#include "core/IniStorage.h"
#include "core/Security.h"
#include "core/SessionData.h"
#include "core/Workspaces.h"

#include <QFile>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
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

static QByteArray ReadFile(const QString & Path)
{
  QFile F(Path);
  CHECK(F.open(QIODevice::ReadOnly));
  return F.readAll();
}

static QSet<QString> KeySet(const IniFile & File, const QString & Section)
{
  const QStringList Keys = File.Keys(Section);
  return QSet<QString>(Keys.begin(), Keys.end());
}

// As written by WinSCP 6.x: sites, a workspace with a link, an ad-hoc
// session and a link to a site that was deleted meanwhile
static QByteArray WinSCPIni()
{
  QByteArray AdHocPassword = Security::EncryptPassword(QStringLiteral("adhocpw"), QStringLiteral("bobadhoc.example.com"));
  return
    "[Sessions\\Default%20Settings]\r\n"
    "Compression=1\r\n"
    "SomeFutureKey=42\r\n"
    "\r\n"
    "[Sessions\\Work/Server]\r\n"
    "HostName=server.example.com\r\n"
    "UserName=martin\r\n"
    "RemoteDirectory=/home/martin\r\n"
    "\r\n"
    "[Sessions\\Old%20WS/0000]\r\n"
    "IsWorkspace=1\r\n"
    "Link=Work/Server\r\n"
    "UserName=martin\r\n"
    "LocalDirectory=C:%5CUsers%5Cmartin\r\n"
    "RemoteDirectory=/var/www\r\n"
    "\r\n"
    "[Sessions\\Old%20WS/0001]\r\n"
    "HostName=adhoc.example.com\r\n"
    "UserName=bob\r\n"
    "IsWorkspace=1\r\n"
    "Password=" + AdHocPassword + "\r\n"
    "RemoteDirectory=/tmp\r\n"
    "\r\n"
    "[Sessions\\Old%20WS/0002]\r\n"
    "IsWorkspace=1\r\n"
    "Link=Deleted%20site\r\n"
    "\r\n"
    "[Sessions\\Old%20WS/0003]\r\n"
    "HostName=named.example.com\r\n"
    "IsWorkspace=1\r\n"
    "NameOverride=Named%20ad-hoc\r\n"
    "\r\n"
    "[Sessions\\Work/Old%20link%20only]\r\n"
    "HostName=other.example.com\r\n";
}

static void TestLoadWinSCPWorkspace(const QString & Dir)
{
  QString FileName = Dir + QStringLiteral("/winscp.ini");
  WriteFile(FileName, WinSCPIni());
  IniFile File;
  CHECK(File.Load(FileName));
  StoredSessionList Sessions;
  Sessions.Load(File);

  CHECK(Workspaces::WorkspaceNames(Sessions) == QStringList{QStringLiteral("Old WS")});
  CHECK(Workspaces::IsWorkspace(Sessions, QStringLiteral("Old WS")));
  CHECK(Workspaces::IsWorkspace(Sessions, QStringLiteral("old ws")));
  CHECK(!Workspaces::IsFolder(Sessions, QStringLiteral("Old WS")));
  CHECK(Workspaces::IsFolder(Sessions, QStringLiteral("Work")));
  CHECK(!Workspaces::IsWorkspace(Sessions, QStringLiteral("Work")));
  CHECK(!Workspaces::IsWorkspace(Sessions, QStringLiteral("Old")));
  CHECK(Workspaces::HasAnyWorkspace(Sessions));

  QList<SessionData> Tabs = Workspaces::LoadWorkspace(Sessions, QStringLiteral("Old WS"));
  // the link to the deleted site is skipped
  CHECK(Tabs.size() == 3);
  if (Tabs.size() == 3)
  {
    // link: the stored site with the workspace state
    CHECK(Tabs[0].Name == QStringLiteral("Work/Server"));
    CHECK(Tabs[0].HostName == QStringLiteral("server.example.com"));
    CHECK(Tabs[0].UserName == QStringLiteral("martin"));
    CHECK(Tabs[0].RemoteDirectory == QStringLiteral("/var/www"));
    CHECK(Tabs[0].LocalDirectory == QStringLiteral("C:\\Users\\martin"));
    CHECK(!Tabs[0].IsWorkspace);
    CHECK(Tabs[0].Link.isEmpty());
    CHECK(Tabs[0].Compression);   // from Default Settings
    // ad-hoc session without name
    CHECK(Tabs[1].Name.isEmpty());
    CHECK(Tabs[1].HostName == QStringLiteral("adhoc.example.com"));
    CHECK(Tabs[1].UserName == QStringLiteral("bob"));
    CHECK(Tabs[1].Password == QStringLiteral("adhocpw"));
    CHECK(Tabs[1].RemoteDirectory == QStringLiteral("/tmp"));
    CHECK(Tabs[1].IsWorkspace);
    CHECK(Tabs[1].SessionName() == QStringLiteral("bob@adhoc.example.com"));
    // ad-hoc session with a name
    CHECK(Tabs[2].Name == QStringLiteral("Named ad-hoc"));
    CHECK(Tabs[2].HostName == QStringLiteral("named.example.com"));
  }
  CHECK(Workspaces::FolderOrWorkspaceList(Sessions, QStringLiteral("Old WS")) ==
    (QStringList{QStringLiteral("Work/Server"), QStringLiteral("bob@adhoc.example.com"), QStringLiteral("Named ad-hoc")}));

  // a site folder opens all its sites
  QList<SessionData> Folder = Workspaces::FolderOrWorkspace(Sessions, QStringLiteral("Work"));
  CHECK(Folder.size() == 2);

  // a link without state keeps the site's directories (pre-5.6.4 workspaces)
  auto Link = std::make_shared<SessionData>(Sessions.DefaultSettings());
  Link->Name = QStringLiteral("Bare/0000");
  Link->IsWorkspace = true;
  Link->Link = QStringLiteral("Work/Server");
  Sessions.Add(Link);
  QList<SessionData> Bare = Workspaces::LoadWorkspace(Sessions, QStringLiteral("Bare"));
  CHECK((Bare.size() == 1) && (Bare.value(0).RemoteDirectory == QStringLiteral("/home/martin")));
}

static SessionData StoredTab(StoredSessionList & Sessions)
{
  // tab opened from a stored site, browsing elsewhere now
  SessionData Data = *Sessions.FindByName(QStringLiteral("Work/Server"));
  Data.RemoteDirectory = QStringLiteral("/srv/data");
  Data.LocalDirectory = QStringLiteral("/home/me/data");
  Data.Password = QStringLiteral("typedpw");
  return Data;
}

static SessionData AdHocTab(const QString & Name)
{
  SessionData Data;
  Data.Name = Name;
  Data.HostName = QStringLiteral("adhoc.example.com");
  Data.UserName = QStringLiteral("eve");
  Data.Password = QStringLiteral("evepw");
  Data.PortNumber = 2200;
  Data.RemoteDirectory = QStringLiteral("/home/eve");
  Data.LocalDirectory = QStringLiteral("/home/me");
  return Data;
}

static void TestSaveWorkspace(const QString & Dir)
{
  QString FileName = Dir + QStringLiteral("/save.ini");
  WriteFile(FileName, WinSCPIni());
  IniFile File;
  CHECK(File.Load(FileName));
  StoredSessionList Sessions;
  Sessions.Load(File);

  QList<SessionData> Tabs;
  Tabs << StoredTab(Sessions) << AdHocTab(QString()) << AdHocTab(QStringLiteral("Gone site"));

  Workspaces::PasswordOptions Options = Workspaces::SavePasswordOptions(Sessions, Tabs);
  CHECK(Options.CanSavePasswords);
  CHECK(Options.NotRecommended);
  CHECK(!Options.SavePasswords);

  Workspaces::SaveWorkspace(Sessions, QStringLiteral("My Workspace"), Tabs, false);
  Sessions.Save(File);
  CHECK(File.Save(FileName));

  QByteArray Text = ReadFile(FileName);
  if (qEnvironmentVariableIsSet("NIXSCP_TEST_VERBOSE"))
  {
    std::printf("%s\n", Text.constData());
  }
  CHECK(Text.contains("[Sessions\\My%20Workspace/0000]"));
  CHECK(Text.contains("[Sessions\\My%20Workspace/0001]"));
  CHECK(Text.contains("[Sessions\\My%20Workspace/0002]"));
  CHECK(!Text.contains("[Sessions\\My%20Workspace/0003]"));

  // the link entry has exactly the keys WinSCP writes
  QString LinkSection = QStringLiteral("Sessions\\My%20Workspace/0000");
  CHECK(KeySet(File, LinkSection) == (QSet<QString>{QStringLiteral("IsWorkspace"), QStringLiteral("Link"),
    QStringLiteral("LocalDirectory"), QStringLiteral("RemoteDirectory")}));
  CHECK(File.ReadRaw(LinkSection, QStringLiteral("IsWorkspace")) == QStringLiteral("1"));
  CHECK(File.ReadRaw(LinkSection, QStringLiteral("Link")) == QStringLiteral("Work/Server"));
  CHECK(File.ReadRaw(LinkSection, QStringLiteral("RemoteDirectory")) == QStringLiteral("/srv/data"));
  CHECK(File.ReadRaw(LinkSection, QStringLiteral("LocalDirectory")) == QStringLiteral("/home/me/data"));

  // ad-hoc entries: full settings, no password, NameOverride only when named
  QString AdHocSection = QStringLiteral("Sessions\\My%20Workspace/0001");
  CHECK(File.ReadRaw(AdHocSection, QStringLiteral("HostName")) == QStringLiteral("adhoc.example.com"));
  CHECK(File.ReadRaw(AdHocSection, QStringLiteral("UserName")) == QStringLiteral("eve"));
  CHECK(File.ReadRaw(AdHocSection, QStringLiteral("PortNumber")) == QStringLiteral("2200"));
  CHECK(File.ReadRaw(AdHocSection, QStringLiteral("IsWorkspace")) == QStringLiteral("1"));
  CHECK(File.ReadRaw(AdHocSection, QStringLiteral("RemoteDirectory")) == QStringLiteral("/home/eve"));
  CHECK(!KeySet(File, AdHocSection).contains(QStringLiteral("Password")));
  CHECK(!KeySet(File, AdHocSection).contains(QStringLiteral("NameOverride")));
  CHECK(!KeySet(File, AdHocSection).contains(QStringLiteral("Link")));
  QString NamedSection = QStringLiteral("Sessions\\My%20Workspace/0002");
  CHECK(File.ReadRaw(NamedSection, QStringLiteral("NameOverride")) == QStringLiteral("Gone%20site"));
  CHECK(!KeySet(File, NamedSection).contains(QStringLiteral("Password")));

  // other sites and workspaces are untouched
  CHECK(Text.contains("[Sessions\\Old%20WS/0001]"));
  CHECK(Text.contains("[Sessions\\Work/Server]"));

  // round trip
  IniFile File2;
  CHECK(File2.Load(FileName));
  StoredSessionList Sessions2;
  Sessions2.Load(File2);
  CHECK(Workspaces::WorkspaceNames(Sessions2) == (QStringList{QStringLiteral("My Workspace"), QStringLiteral("Old WS")}));
  QList<SessionData> Loaded = Workspaces::LoadWorkspace(Sessions2, QStringLiteral("My Workspace"));
  CHECK(Loaded.size() == 3);
  if (Loaded.size() == 3)
  {
    CHECK(Loaded[0].Name == QStringLiteral("Work/Server"));
    CHECK(Loaded[0].HostName == QStringLiteral("server.example.com"));
    CHECK(Loaded[0].RemoteDirectory == QStringLiteral("/srv/data"));
    CHECK(Loaded[0].LocalDirectory == QStringLiteral("/home/me/data"));
    CHECK(Loaded[0].Password.isEmpty());
    CHECK(!Loaded[0].IsWorkspace);
    CHECK(Loaded[1].Name.isEmpty());
    CHECK(Loaded[1].HostName == QStringLiteral("adhoc.example.com"));
    CHECK(Loaded[1].PortNumber == 2200);
    CHECK(Loaded[1].Password.isEmpty());
    CHECK(Loaded[1].IsWorkspace);
    CHECK(Loaded[1].RemoteDirectory == QStringLiteral("/home/eve"));
    CHECK(Loaded[2].Name == QStringLiteral("Gone site"));
  }

  // re-saving the loaded workspace keeps its form: link, ad-hoc, named ad-hoc
  QList<SessionData> Again = Loaded;
  Workspaces::SaveWorkspace(Sessions2, QStringLiteral("My Workspace"), Again, false);
  CHECK(Sessions2.FindByName(QStringLiteral("My Workspace/0000"))->Link == QStringLiteral("Work/Server"));
  CHECK(Sessions2.FindByName(QStringLiteral("My Workspace/0001"))->Link.isEmpty());
  CHECK(Sessions2.FindByName(QStringLiteral("My Workspace/0001"))->NameOverride.isEmpty());
  CHECK(Sessions2.FindByName(QStringLiteral("My Workspace/0002"))->NameOverride == QStringLiteral("Gone site"));

  // passwords of ad-hoc sessions with "Save passwords"; never in links
  Workspaces::SaveWorkspace(Sessions2, QStringLiteral("My Workspace"), Tabs.mid(0, 2), true);
  Sessions2.Save(File2);
  CHECK(File2.Save(FileName));
  IniFile File3;
  CHECK(File3.Load(FileName));
  CHECK(!File3.SectionExists(QStringLiteral("Sessions\\My%20Workspace/0002")));
  CHECK(!KeySet(File3, LinkSection).contains(QStringLiteral("Password")));
  QString Encrypted = File3.ReadRaw(AdHocSection, QStringLiteral("Password"));
  CHECK(!Encrypted.isEmpty());
  CHECK(Security::DecryptPassword(Encrypted.toLatin1(), QStringLiteral("eveadhoc.example.com")) == QStringLiteral("evepw"));
  StoredSessionList Sessions3;
  Sessions3.Load(File3);
  QList<SessionData> WithPasswords = Workspaces::LoadWorkspace(Sessions3, QStringLiteral("My Workspace"));
  CHECK((WithPasswords.size() == 2) && (WithPasswords.value(1).Password == QStringLiteral("evepw")));

  // delete
  CHECK(Workspaces::DeleteWorkspace(Sessions3, QStringLiteral("My Workspace")) == 2);
  Sessions3.Save(File3);
  CHECK(!File3.SectionExists(QStringLiteral("Sessions\\My%20Workspace/0000")));
  CHECK(!File3.SectionExists(QStringLiteral("Sessions\\My%20Workspace/0001")));
  CHECK(File3.SectionExists(QStringLiteral("Sessions\\Old%20WS/0000")));
  CHECK(Workspaces::WorkspaceNames(Sessions3) == QStringList{QStringLiteral("Old WS")});
}

static void TestHelpers()
{
  CHECK(Workspaces::EntryName(QStringLiteral("WS"), 0) == QStringLiteral("WS/0000"));
  CHECK(Workspaces::EntryName(QStringLiteral("WS"), 10) == QStringLiteral("WS/000A"));
  CHECK(Workspaces::EntryName(QStringLiteral("WS"), 4096) == QStringLiteral("WS/1000"));

  StoredSessionList Sessions;
  auto Site = std::make_shared<SessionData>();
  Site->Name = QStringLiteral("Folder/Site");
  Site->HostName = QStringLiteral("example.com");
  Sessions.Add(Site);
  CHECK(Workspaces::ValidateWorkspaceName(Sessions, QStringLiteral("Good name")).isEmpty());
  CHECK(Workspaces::ValidateWorkspaceName(Sessions, QStringLiteral("a/b")) == QStringLiteral("Invalid name 'a/b'. Name cannot contain '/'."));
  CHECK(Workspaces::ValidateWorkspaceName(Sessions, QStringLiteral("Folder")) == QStringLiteral("'Folder' is site folder, not workspace."));

  // FindSame: only named, non-workspace sessions that exist
  SessionData Data = *Site;
  CHECK(Workspaces::FindSame(Sessions, Data) != nullptr);
  Data.IsWorkspace = true;
  CHECK(Workspaces::FindSame(Sessions, Data) == nullptr);
  Data.IsWorkspace = false;
  Data.Name.clear();
  CHECK(Workspaces::FindSame(Sessions, Data) == nullptr);

  // passwords of stored sites do not count; anonymous sessions are not "not recommended"
  QList<SessionData> Tabs{*Site};
  Tabs[0].Password = QStringLiteral("x");
  CHECK(!Workspaces::SavePasswordOptions(Sessions, Tabs).CanSavePasswords);
  SessionData Anonymous;
  Anonymous.HostName = QStringLiteral("ftp.example.com");
  Anonymous.UserName = QStringLiteral("anonymous");
  Anonymous.Password = QStringLiteral("anonymous@example.com");
  Tabs.append(Anonymous);
  Workspaces::PasswordOptions Options = Workspaces::SavePasswordOptions(Sessions, Tabs);
  CHECK(Options.CanSavePasswords && !Options.NotRecommended && Options.SavePasswords);

  // circular links do not hang
  auto A = std::make_shared<SessionData>();
  A->Name = QStringLiteral("A");
  A->Link = QStringLiteral("B");
  auto B = std::make_shared<SessionData>();
  B->Name = QStringLiteral("B");
  B->Link = QStringLiteral("A");
  Sessions.Add(A);
  Sessions.Add(B);
  CHECK(Workspaces::ResolveWorkspaceData(Sessions, A) == nullptr);

  // WorkspaceName: last, auto, default
  Configuration & Config = Configuration::Instance();
  Workspaces::SetLastWorkspace(QString());
  Config.AutoWorkspace.clear();
  CHECK(Workspaces::WorkspaceName(Config) == QStringLiteral("My Workspace"));
  Config.AutoWorkspace = QStringLiteral("Auto");
  CHECK(Workspaces::WorkspaceName(Config) == QStringLiteral("Auto"));
  Workspaces::SetLastWorkspace(QStringLiteral("Last"));
  CHECK(Workspaces::WorkspaceName(Config) == QStringLiteral("Last"));
}

static std::shared_ptr<SessionData> Site(const QString & Name, const QString & Host)
{
  auto Data = std::make_shared<SessionData>();
  Data->Name = Name;
  Data->HostName = Host;
  return Data;
}

// X-M9, L38: a workspace never takes sites of the same name with it
static void TestSitesSharingTheName()
{
  StoredSessionList Sessions;
  Sessions.Add(Site(QStringLiteral("Prod/web"), QStringLiteral("web.example.com")));
  Sessions.Add(Site(QStringLiteral("Prod/0000"), QStringLiteral("zero.example.com")));
  SessionData Tab;
  Tab.HostName = QStringLiteral("tab.example.com");
  Tab.UserName = QStringLiteral("me");

  // an auto-saved workspace is not validated
  Workspaces::SaveWorkspace(Sessions, QStringLiteral("Prod"), {Tab}, false);
  CHECK(Sessions.FindByName(QStringLiteral("Prod/web")) != nullptr);
  auto Zero = Sessions.FindByName(QStringLiteral("Prod/0000"));
  CHECK((Zero != nullptr) && (Zero->HostName == QStringLiteral("zero.example.com")) && !Zero->IsWorkspace);
  CHECK(Workspaces::IsWorkspace(Sessions, QStringLiteral("Prod")));
  CHECK(Workspaces::IsFolder(Sessions, QStringLiteral("Prod")));
  // only its tabs are opened
  QList<SessionData> Tabs = Workspaces::LoadWorkspace(Sessions, QStringLiteral("Prod"));
  CHECK((Tabs.size() == 1) && (Tabs.value(0).HostName == QStringLiteral("tab.example.com")));
  // saved again, also with a site sorted after the workspace entries
  Sessions.Add(Site(QStringLiteral("Prod/zzz"), QStringLiteral("z.example.com")));
  Workspaces::SaveWorkspace(Sessions, QStringLiteral("Prod"), {Tab, Tab}, false);
  CHECK(Workspaces::LoadWorkspace(Sessions, QStringLiteral("Prod")).size() == 2);
  for (const QString & Name : {QStringLiteral("Prod/web"), QStringLiteral("Prod/0000"), QStringLiteral("Prod/zzz")})
  {
    CHECK(Sessions.FindByName(Name) != nullptr);
  }
  // the dialog refuses the name of a site folder, whatever comes first in it
  CHECK(!Workspaces::ValidateWorkspaceName(Sessions, QStringLiteral("Prod")).isEmpty());
  CHECK(Workspaces::DeleteWorkspace(Sessions, QStringLiteral("Prod")) == 2);
  CHECK(!Workspaces::IsWorkspace(Sessions, QStringLiteral("Prod")));
  CHECK(Sessions.FindByName(QStringLiteral("Prod/web")) != nullptr);
  CHECK(Sessions.Sessions().size() == 3);
}

// L37: rename and move do not leave links dangling
static void TestLinksFollowRename()
{
  StoredSessionList Sessions;
  auto Server = Site(QStringLiteral("Work/Server"), QStringLiteral("server.example.com"));
  auto Other = Site(QStringLiteral("Workshop/Other"), QStringLiteral("other.example.com"));
  Sessions.Add(Server);
  Sessions.Add(Other);
  SessionData T1 = *Server;
  SessionData T2 = *Other;
  Workspaces::SaveWorkspace(Sessions, QStringLiteral("WS"), {T1, T2}, false);
  CHECK(Workspaces::LoadWorkspace(Sessions, QStringLiteral("WS")).size() == 2);

  // a site
  SessionData Copy = *Server;
  Sessions.Remove(Server->Name);
  Sessions.NewSession(QStringLiteral("Work/Renamed"), Copy);
  CHECK(Workspaces::LoadWorkspace(Sessions, QStringLiteral("WS")).size() == 1);
  CHECK(Workspaces::UpdateLinks(Sessions, QStringLiteral("Work/Server"), QStringLiteral("Work/Renamed")) == 1);
  CHECK(Workspaces::LoadWorkspace(Sessions, QStringLiteral("WS")).size() == 2);
  // a folder: not the sites of a folder of a similar name
  Sessions.RenameFolder(QStringLiteral("Work"), QStringLiteral("Job"));
  CHECK(Workspaces::UpdateLinks(Sessions, QStringLiteral("Work"), QStringLiteral("Job"), true) == 1);
  QList<SessionData> Tabs = Workspaces::LoadWorkspace(Sessions, QStringLiteral("WS"));
  CHECK((Tabs.size() == 2) && (Tabs.value(0).Name == QStringLiteral("Job/Renamed")) && (Tabs.value(1).Name == QStringLiteral("Workshop/Other")));
}

// X-M17: "Do not save passwords" means all of them
static void TestNoPasswordsSaved(const QString & Dir)
{
  SessionData Tab;
  Tab.HostName = QStringLiteral("adhoc.example.com");
  Tab.UserName = QStringLiteral("me");
  Tab.ProxyMethod = pmHTTP;
  Tab.ProxyHost = QStringLiteral("proxy.example.com");
  Tab.ProxyUsername = QStringLiteral("proxyuser");
  CHECK(!Workspaces::HasAnySessionPassword(Tab));
  Tab.ProxyPassword = QStringLiteral("proxypw");
  CHECK(Workspaces::HasAnySessionPassword(Tab));
  Tab.ProxyPassword.clear();
  Tab.S3SessionToken = QStringLiteral("s3token");
  CHECK(Workspaces::HasAnySessionPassword(Tab));
  Tab.ProxyPassword = QStringLiteral("proxypw");
  Tab.Password = QStringLiteral("sessionpw");
  Tab.TunnelPassword = QStringLiteral("tunnelpw");

  StoredSessionList Sessions;
  CHECK(Workspaces::SavePasswordOptions(Sessions, {Tab}).CanSavePasswords);
  Workspaces::SaveWorkspace(Sessions, QStringLiteral("NoPw"), {Tab}, false);
  auto Entry = Sessions.FindByName(QStringLiteral("NoPw/0000"));
  CHECK(Entry != nullptr);
  if (Entry != nullptr)
  {
    CHECK(!Workspaces::HasAnySessionPassword(*Entry));
  }
  QString FileName = Dir + QStringLiteral("/nopw.ini");
  IniFile File;
  Sessions.Save(File);
  CHECK(File.Save(FileName));
  QByteArray Content = ReadFile(FileName);
  CHECK(!Content.contains("s3token"));
  CHECK(!Content.contains("ProxyPasswordEnc"));
  // kept when wanted
  Workspaces::SaveWorkspace(Sessions, QStringLiteral("WithPw"), {Tab}, true);
  Entry = Sessions.FindByName(QStringLiteral("WithPw/0000"));
  CHECK((Entry != nullptr) && (Entry->ProxyPassword == QStringLiteral("proxypw")) && (Entry->S3SessionToken == QStringLiteral("s3token")));
}

// F-04: local and disconnected tabs survive saving and loading
static void TestLocalTabs(const QString & Dir)
{
  StoredSessionList Sessions;
  auto Server = Site(QStringLiteral("Server"), QStringLiteral("server.example.com"));
  Sessions.Add(Server);
  SessionData Local;
  Local.LocalDirectory = QStringLiteral("/home/me/left");
  Local.OtherLocalDirectory = QStringLiteral("/home/me/right");
  CHECK(Local.IsLocalBrowser() && Local.CanOpen() && !Local.CanLogin());
  SessionData Tab = *Server;
  Tab.RemoteDirectory = QStringLiteral("/var/www");
  Workspaces::SaveWorkspace(Sessions, QStringLiteral("Mixed"), {Tab, Local}, false);
  CHECK(Sessions.CanOpen(*Sessions.FindByName(QStringLiteral("Mixed/0001"))));

  QString FileName = Dir + QStringLiteral("/mixed.ini");
  IniFile File;
  Sessions.Save(File);
  CHECK(File.Save(FileName));
  IniFile Read;
  CHECK(Read.Load(FileName));
  StoredSessionList Loaded;
  Loaded.Load(Read);
  QList<SessionData> Tabs = Workspaces::LoadWorkspace(Loaded, QStringLiteral("Mixed"));
  CHECK(Tabs.size() == 2);
  if (Tabs.size() == 2)
  {
    CHECK(Tabs[0].HostName == QStringLiteral("server.example.com"));
    CHECK(Tabs[0].RemoteDirectory == QStringLiteral("/var/www"));
    CHECK(Tabs[1].IsLocalBrowser() && Tabs[1].HostName.isEmpty());
    CHECK(Tabs[1].LocalDirectory == QStringLiteral("/home/me/left"));
    CHECK(Tabs[1].OtherLocalDirectory == QStringLiteral("/home/me/right"));
  }
  CHECK(Workspaces::FolderOrWorkspaceList(Loaded, QStringLiteral("Mixed")).value(1) ==
    QStringLiteral("left \u2013 right"));
}

// F-03: LastStoredSession goes through the same place as the Login dialog's
static void TestLastStoredSession(const QString & Dir)
{
  Configuration & Config = Configuration::Instance();
  CHECK(Config.LastStoredSession().isEmpty());
  Config.SetLastStoredSession(QStringLiteral("My WS"));
  CHECK(Config.LastStoredSession() == QStringLiteral("My WS"));
  CHECK(Config.Save());
  QByteArray Content = ReadFile(Dir + QStringLiteral("/nixscp.ini"));
  CHECK(Content.contains("LastStoredSession=My%20WS"));
  Config.SetLastStoredSession(QString());
  CHECK(Config.LastStoredSession().isEmpty());
}

int main()
{
  QStandardPaths::setTestModeEnabled(true);
  QTemporaryDir Dir;
  CHECK(Dir.isValid());
  Configuration::Instance().SetIniFileName(Dir.path() + QStringLiteral("/nixscp.ini"));

  TestLoadWinSCPWorkspace(Dir.path());
  TestSaveWorkspace(Dir.path());
  TestHelpers();
  TestSitesSharingTheName();
  TestLinksFollowRename();
  TestNoPasswordsSaved(Dir.path());
  TestLocalTabs(Dir.path());
  TestLastStoredSession(Dir.path());

  if (Failures == 0)
  {
    std::printf("All workspace tests passed\n");
  }
  return (Failures == 0) ? 0 : 1;
}
