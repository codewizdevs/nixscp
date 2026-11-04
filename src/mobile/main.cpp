// NixSCP for Android: the touch interface (qml/) on top of the core.
// Saved sites (search, favourites), the phone's and the server's files side
// by side (or as tabs), transfers with progress. The encrypted configuration
// and NixSCP Pro sign-in follow.
#include "core/Configuration.h"
#include "core/CloudSync.h"
#include "core/TeamSync.h"
#include "mobile/AndroidFiles.h"
#include "mobile/AppLock.h"
#include "mobile/ProAccount.h"
#include "mobile/TeamsModel.h"
#include "mobile/KeyStore.h"
#include "mobile/LocalBrowser.h"
#include "mobile/MobileSettings.h"
#include "mobile/MobileUserInterface.h"
#include "mobile/RemoteSession.h"
#include "mobile/SessionManager.h"
#include "mobile/SiteChecker.h"
#include "mobile/MobileQueue.h"
#include "mobile/SitesModel.h"
#include "mobile/SystemInsets.h"

#include <QFontDatabase>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>

#include <libssh/libssh.h>

#ifdef Q_OS_ANDROID
#include <sys/system_properties.h>

// The Android emulator's GPU translation crashes when Qt stores compiled
// shaders (QOpenGLProgramBinaryCache::save); phones are not affected
static bool RunningInEmulator()
{
  char Value[PROP_VALUE_MAX] = {};
  return ((__system_property_get("ro.boot.qemu", Value) > 0) && (Value[0] == '1')) ||
    ((__system_property_get("ro.kernel.qemu", Value) > 0) && (Value[0] == '1'));
}
#endif

int main(int argc, char * argv[])
{
#ifdef Q_OS_ANDROID
  if (RunningInEmulator())
  {
    QCoreApplication::setAttribute(Qt::AA_DisableShaderDiskCache);
  }
#endif
  QGuiApplication App(argc, argv);
  QGuiApplication::setApplicationName(QStringLiteral("NixSCP"));
  QGuiApplication::setOrganizationName(QStringLiteral("NixSCP"));
  QGuiApplication::setApplicationVersion(QStringLiteral(PROJECT_VERSION));
  ssh_init();

  // loaded by the lock: an encrypted configuration (NixSCP Pro) is opened first
  Configuration & Config = Configuration::Instance();
  // decrypted SSH keys of an earlier run
  KeyStore::CleanTemporary();

  QQuickStyle::setStyle(QStringLiteral("Material"));
  MobileUserInterface UI;
  MobileSettings Settings;
  SitesModel Sites(&Settings);
  MobileQueue Transfers;
  SessionManager Sessions(&UI, &Settings, &Transfers);
  SiteChecker Checker;
  KeyStore Keys;
  SystemInsets Insets;
  AndroidFiles Files;
  LocalBrowser Phone;
  ProAccount Pro(&Files);
  TeamsModel Teams;
  // the start, once the configuration can be read
  AppLock Lock(Config.IniFileName(), [&]()
  {
    Config.Load();
    Settings.ApplyToConfiguration();
    // NixSCP Pro: settings sync and teams (inactive until signed in)
    CloudSync::Instance().Initialize(Config);
    TeamSync::Instance().Initialize(Config, CloudSync::Instance());
    Sites.MigrateKeys();
    Sites.reload();
    Pro.Start();
    Teams.Start();
    KeyStore::SealAll();
  });
  // pulled settings, team sites: the list again; a key just set up: the SSH keys encrypted
  QObject::connect(&Pro, &ProAccount::sitesChanged, &Sites, &SitesModel::reload);
  QObject::connect(&Pro, &ProAccount::changed, &Keys, []() { KeyStore::SealAll(); });

  QQmlApplicationEngine Engine;
  Engine.rootContext()->setContextProperty(QStringLiteral("ui"), &UI);
  Engine.rootContext()->setContextProperty(QStringLiteral("sites"), &Sites);
  // "session" is the current tab: QML's bindings follow it when it changes
  Engine.rootContext()->setContextProperty(QStringLiteral("sessions"), &Sessions);
  Engine.rootContext()->setContextProperty(QStringLiteral("session"), Sessions.current());
  QObject::connect(&Sessions, &SessionManager::currentChanged, &Engine, [&Engine, &Sessions]()
  {
    Engine.rootContext()->setContextProperty(QStringLiteral("session"), Sessions.current());
  });
  Engine.rootContext()->setContextProperty(QStringLiteral("insets"), &Insets);
  Engine.rootContext()->setContextProperty(QStringLiteral("androidFiles"), &Files);
  Engine.rootContext()->setContextProperty(QStringLiteral("phone"), &Phone);
  Engine.rootContext()->setContextProperty(QStringLiteral("settings"), &Settings);
  Engine.rootContext()->setContextProperty(QStringLiteral("keys"), &Keys);
  Engine.rootContext()->setContextProperty(QStringLiteral("transfers"), &Transfers);
  Engine.rootContext()->setContextProperty(QStringLiteral("checker"), &Checker);
  Engine.rootContext()->setContextProperty(QStringLiteral("lock"), &Lock);
  Engine.rootContext()->setContextProperty(QStringLiteral("pro"), &Pro);
  Engine.rootContext()->setContextProperty(QStringLiteral("teams"), &Teams);
  Engine.rootContext()->setContextProperty(QStringLiteral("appVersion"), QStringLiteral(PROJECT_VERSION));
  // a fixed-width font for paths, the console and the editor ("monospace"
  // is not one on Android)
  QString MonoFont = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
  const QStringList Families = QFontDatabase::families();
  for (const QString & Candidate : {QStringLiteral("Droid Sans Mono"), QStringLiteral("Cutive Mono")})
  {
    if (Families.contains(Candidate))
    {
      MonoFont = Candidate;
      break;
    }
  }
  Engine.rootContext()->setContextProperty(QStringLiteral("monoFont"), MonoFont);
  QObject::connect(&Engine, &QQmlApplicationEngine::objectCreationFailed, &App, []() { QCoreApplication::exit(1); },
    Qt::QueuedConnection);
  Engine.loadFromModule(QStringLiteral("NixSCP"), QStringLiteral("Main"));
  // the lock page, or the start right away
  Lock.Begin();
  // the intent NixSCP was started with (shared files, a site's shortcut)
  Files.Start();
  int Result = QGuiApplication::exec();
  ssh_finalize();
  return Result;
}
