// Offscreen GUI test of the NixSCP Pro "session" slot payload (gui/CloudSyncUi):
// open tabs saved as a workspace (links to stored sites, complete ad-hoc
// sites without passwords) and reopened against this computer's sites.
// NIXSCP_GUI_TESTS.
#ifndef NIXSCP_GUI_TESTS

#include <cstdio>

int main()
{
  std::printf("SKIP: test_cloudsession needs NIXSCP_GUI_TESTS (Qt Widgets + the GUI sources)\n");
  return 0;
}

#else

#include "core/Configuration.h"
#include "core/IniStorage.h"
#include "core/SessionData.h"
#include "gui/CloudSyncUi.h"

#include <QApplication>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <cstdio>
#include <memory>

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); Failures++; } } while (0)

static void TestRoundTrip(const QString & ExistingDir)
{
  Configuration & Config = Configuration::Instance();
  SessionData Site;
  Site.Name = QStringLiteral("Work/Server");
  Site.HostName = QStringLiteral("srv.example.com");
  Site.UserName = QStringLiteral("me");
  Site.FSProtocol = fsSFTP;
  Config.StoredSessions().Add(std::make_shared<SessionData>(Site));
  CHECK(Config.Save());
  const int SiteCount = Config.StoredSessions().Sessions().size();

  SessionData Tab1 = Site;
  Tab1.RemoteDirectory = QStringLiteral("/var/www");
  Tab1.LocalDirectory = ExistingDir;
  SessionData Tab2;
  Tab2.HostName = QStringLiteral("adhoc.example.com");
  Tab2.UserName = QStringLiteral("bob");
  Tab2.Password = QStringLiteral("secret");
  Tab2.FSProtocol = fsSFTP;
  Tab2.RemoteDirectory = QStringLiteral("/tmp");
  Tab2.LocalDirectory = QStringLiteral("/nonexistent/nixscp/dir");

  Config.AutoSaveWorkspacePasswords = false;
  QByteArray Payload = CloudSyncUi::SessionPayload({Tab1, Tab2});
  CHECK(Payload.contains("Link=Work/Server"));
  CHECK(Payload.contains("adhoc.example.com"));
  CHECK(!Payload.contains("Password"));
  // Nothing but the two tab sections
  IniFile Parsed;
  CHECK(Parsed.LoadFromData(Payload));
  CHECK(Parsed.Sections().size() == 2);
  // The user's own site list is untouched
  CHECK(Config.StoredSessions().Sessions().size() == SiteCount);
  CHECK(!Config.StoredSessions().IsWorkspace(QStringLiteral("NixSCP Cloud Session")));

  QList<SessionData> Restored = CloudSyncUi::SessionsFromPayload(Payload);
  CHECK(Restored.size() == 2);
  if (Restored.size() == 2)
  {
    CHECK(Restored[0].HostName == QStringLiteral("srv.example.com"));
    CHECK(Restored[0].UserName == QStringLiteral("me"));
    CHECK(Restored[0].RemoteDirectory == QStringLiteral("/var/www"));
    CHECK(Restored[0].LocalDirectory == ExistingDir);
    CHECK(!Restored[0].IsWorkspace);
    CHECK(Restored[1].HostName == QStringLiteral("adhoc.example.com"));
    CHECK(Restored[1].RemoteDirectory == QStringLiteral("/tmp"));
    // Another computer's local directory that does not exist here
    CHECK(Restored[1].LocalDirectory.isEmpty());
    CHECK(Restored[1].Password.isEmpty());
  }

  // With "save passwords" of the auto-saved workspace, ad-hoc passwords travel
  Config.AutoSaveWorkspacePasswords = true;
  QList<SessionData> WithPassword = CloudSyncUi::SessionsFromPayload(CloudSyncUi::SessionPayload({Tab2}));
  CHECK((WithPassword.size() == 1) && (WithPassword.value(0).Password == QStringLiteral("secret")));
  Config.AutoSaveWorkspacePasswords = false;

  // Sections other than the session workspace are ignored (no injected sites)
  QByteArray Injected = Payload + "\n[Sessions\\Evil]\nHostName=evil.example.com\n";
  CHECK(CloudSyncUi::SessionsFromPayload(Injected).size() == 2);
  CHECK(Config.StoredSessions().FindByName(QStringLiteral("Evil")) == nullptr);

  // A tab of a site that does not exist on this computer is skipped
  Config.StoredSessions().Remove(QStringLiteral("Work/Server"));
  QList<SessionData> Dangling = CloudSyncUi::SessionsFromPayload(Payload);
  CHECK(Dangling.size() == 1);
  CHECK(Dangling.value(0).HostName == QStringLiteral("adhoc.example.com"));

  // No tabs
  CHECK(CloudSyncUi::SessionPayload({}).isEmpty());
  CHECK(CloudSyncUi::SessionsFromPayload(QByteArray()).isEmpty());
  CHECK(CloudSyncUi::SessionsFromPayload("garbage").isEmpty());
}

int main(int argc, char ** argv)
{
  QApplication App(argc, argv);
  QStandardPaths::setTestModeEnabled(true);
  QTemporaryDir Dir;
  CHECK(Dir.isValid());
  Configuration::Instance().SetIniFileName(Dir.path() + QStringLiteral("/nixscp.ini"));
  Configuration::Instance().Load();

  TestRoundTrip(Dir.path());

  if (Failures == 0)
  {
    std::printf("All cloud session tests passed\n");
  }
  return (Failures == 0) ? 0 : 1;
}

#endif
