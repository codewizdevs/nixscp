// New version check (gui/UpdateCheck) against a stand-in for the GitHub API
// (a local HTTP server): the notice for a newer release, nothing for the same
// or an older one, a skipped version, the daily limit, the setting, drafts /
// pre-releases and errors. NIXSCP_GUI_TESTS
#include "core/Configuration.h"
#include "gui/UpdateCheck.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QMessageBox>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QWidget>

#include <cstdio>
#include <functional>

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)

// Answers every request with Body (HTTP 200) or Status
static QByteArray Body;
static int Status = 200;
static int Requests = 0;

static void Wait(int Ms)
{
  QElapsedTimer Timer;
  Timer.start();
  while (Timer.elapsed() < Ms)
  {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
  }
}

static QMessageBox * Notice()
{
  for (QWidget * W : QApplication::topLevelWidgets())
  {
    auto * Box = qobject_cast<QMessageBox *>(W);
    if ((Box != nullptr) && Box->isVisible() && Box->text().contains(QStringLiteral("is available")))
    {
      return Box;
    }
  }
  return nullptr;
}

static void Click(QMessageBox * Box, const QString & Caption)
{
  for (QAbstractButton * B : Box->buttons())
  {
    if (B->text() == Caption)
    {
      B->click();
    }
  }
  Wait(50);
}

static QByteArray Release(const char * Tag, bool Draft = false, bool Prerelease = false)
{
  return QStringLiteral("{\"tag_name\":\"%1\",\"html_url\":\"https://github.com/codewizdevs/nixscp/releases/tag/%1\","
    "\"draft\":%2,\"prerelease\":%3}").arg(QString::fromLatin1(Tag), Draft ? QStringLiteral("true") : QStringLiteral("false"),
    Prerelease ? QStringLiteral("true") : QStringLiteral("false")).toUtf8();
}

// Start() as at startup, with a fresh "last check"
static QMessageBox * Startup(QWidget * Window)
{
  Configuration::Instance().UpdateLastCheck = QDateTime();
  UpdateCheck::Start(Window);
  Wait(800);
  return Notice();
}

// L39: the daily limit, and a clock that was set back
static void TestCheckDue()
{
  QDateTime Now = QDateTime::currentDateTimeUtc();
  CHECK(UpdateCheck::CheckDue(QDateTime(), Now));
  CHECK(!UpdateCheck::CheckDue(Now.addSecs(-3600), Now));
  CHECK(UpdateCheck::CheckDue(Now.addSecs(-25 * 3600), Now));
  CHECK(UpdateCheck::CheckDue(Now.addDays(400), Now));
}

int main(int argc, char ** argv)
{
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
  {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  qputenv("NIXSCP_UPDATE_DELAY_MS", "10");
  QApplication App(argc, argv);
  TestCheckDue();
  QTemporaryDir Dir;
  Configuration & Config = Configuration::Instance();
  Config.SetIniFileName(Dir.path() + QStringLiteral("/nixscp.ini"));
  Config.Load();

  QTcpServer Server;
  CHECK(Server.listen(QHostAddress::LocalHost));
  QObject::connect(&Server, &QTcpServer::newConnection, &Server, [&Server]()
  {
    QTcpSocket * Socket = Server.nextPendingConnection();
    QObject::connect(Socket, &QTcpSocket::readyRead, Socket, [Socket]()
    {
      if (!Socket->readAll().contains("\r\n\r\n"))
      {
        return;
      }
      Requests++;
      QByteArray Reason = (Status == 200) ? QByteArrayLiteral("OK") : QByteArrayLiteral("Error");
      Socket->write("HTTP/1.1 " + QByteArray::number(Status) + " " + Reason + "\r\nContent-Type: application/json\r\n"
        "Content-Length: " + QByteArray::number(Body.size()) + "\r\nConnection: close\r\n\r\n" + Body);
      Socket->disconnectFromHost();
    });
  });
  qputenv("NIXSCP_UPDATE_URL", QStringLiteral("http://127.0.0.1:%1/releases/latest").arg(Server.serverPort()).toUtf8());

  // version comparison
  CHECK(UpdateCheck::IsNewer(QStringLiteral("v99.0.0"), UpdateCheck::CurrentVersion()));
  CHECK(UpdateCheck::IsNewer(QStringLiteral("1.10.0"), QStringLiteral("1.9.9")));
  CHECK(!UpdateCheck::IsNewer(QStringLiteral("v1.0.0"), QStringLiteral("1.1.0")));
  CHECK(!UpdateCheck::IsNewer(UpdateCheck::CurrentVersion(), UpdateCheck::CurrentVersion()));
  CHECK(!UpdateCheck::IsNewer(QStringLiteral("garbage"), QStringLiteral("1.0.0")));

  QWidget Window;
  Window.show();

  // a newer release: the notice, and the time of the check is kept
  Body = Release("v99.1.0");
  QMessageBox * Box = Startup(&Window);
  CHECK(Box != nullptr);
  CHECK(Config.UpdateLastCheck.isValid());
  if (Box != nullptr)
  {
    CHECK(Box->text().contains(QStringLiteral("99.1.0")));
    CHECK(!Box->isModal());
    Click(Box, QStringLiteral("Remind me later"));
  }
  CHECK(Notice() == nullptr);

  // checked within the last day: no request at all
  int Before = Requests;
  UpdateCheck::Start(&Window);
  Wait(500);
  CHECK(Requests == Before);

  // skipped: not offered again; a still newer one is
  Box = Startup(&Window);
  CHECK(Box != nullptr);
  if (Box != nullptr) Click(Box, QStringLiteral("Skip this version"));
  CHECK(Config.UpdateSkipVersion == QStringLiteral("99.1.0"));
  CHECK(Startup(&Window) == nullptr);
  Body = Release("v99.2.0");
  Box = Startup(&Window);
  CHECK(Box != nullptr);
  if (Box != nullptr) Click(Box, QStringLiteral("Remind me later"));

  // the same, an older version, a draft or a pre-release: nothing
  for (const QByteArray & Answer : QList<QByteArray>{Release(UpdateCheck::CurrentVersion().toLatin1().constData()), Release("v0.9.0"), Release("v99.3.0", true), Release("v99.3.0", false, true)})
  {
    Body = Answer;
    CHECK(Startup(&Window) == nullptr);
  }

  // a server error: nothing, and checked again next time
  Body = QByteArray();
  Status = 500;
  Config.UpdateLastCheck = QDateTime();
  UpdateCheck::Start(&Window);
  Wait(800);
  CHECK(Notice() == nullptr);
  CHECK(!Config.UpdateLastCheck.isValid());
  Status = 200;

  // turned off: no request
  Config.CheckForUpdates = false;
  Body = Release("v99.4.0");
  Before = Requests;
  CHECK(Startup(&Window) == nullptr);
  CHECK(Requests == Before);

  if (Failures == 0)
  {
    std::printf("All update check tests passed\n");
  }
  return (Failures == 0) ? 0 : 1;
}
