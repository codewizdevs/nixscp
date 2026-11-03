// NixSCP for Android: the touch interface (qml/) on top of the core.
// Prototype: saved sites, connect, browse, download, upload, delete, rename,
// new folder. The encrypted configuration and NixSCP Pro sign-in follow.
#include "core/Configuration.h"
#include "mobile/AndroidFiles.h"
#include "mobile/MobileUserInterface.h"
#include "mobile/RemoteSession.h"
#include "mobile/SitesModel.h"
#include "mobile/SystemInsets.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>

#include <libssh/libssh.h>

int main(int argc, char * argv[])
{
  QGuiApplication App(argc, argv);
  QGuiApplication::setApplicationName(QStringLiteral("NixSCP"));
  QGuiApplication::setOrganizationName(QStringLiteral("NixSCP"));
  QGuiApplication::setApplicationVersion(QStringLiteral(PROJECT_VERSION));
  ssh_init();

  Configuration::Instance().Load();

  QQuickStyle::setStyle(QStringLiteral("Material"));
  MobileUserInterface UI;
  SitesModel Sites;
  RemoteSession Session(&UI);
  SystemInsets Insets;
  AndroidFiles Files;

  QQmlApplicationEngine Engine;
  Engine.rootContext()->setContextProperty(QStringLiteral("ui"), &UI);
  Engine.rootContext()->setContextProperty(QStringLiteral("sites"), &Sites);
  Engine.rootContext()->setContextProperty(QStringLiteral("session"), &Session);
  Engine.rootContext()->setContextProperty(QStringLiteral("insets"), &Insets);
  Engine.rootContext()->setContextProperty(QStringLiteral("androidFiles"), &Files);
  Engine.rootContext()->setContextProperty(QStringLiteral("appVersion"), QStringLiteral(PROJECT_VERSION));
  QObject::connect(&Engine, &QQmlApplicationEngine::objectCreationFailed, &App, []() { QCoreApplication::exit(1); },
    Qt::QueuedConnection);
  Engine.loadFromModule(QStringLiteral("NixSCP"), QStringLiteral("Main"));
  int Result = QGuiApplication::exec();
  ssh_finalize();
  return Result;
}
