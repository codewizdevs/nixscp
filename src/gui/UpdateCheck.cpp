#include "gui/UpdateCheck.h"
#include "gui/GuiUserInterface.h"

#include "core/Configuration.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVersionNumber>

#include <functional>

namespace UpdateCheck
{

namespace
{

constexpr int StartDelayMs = 15 * 1000;
constexpr qint64 CheckIntervalSecs = 24 * 60 * 60;
constexpr int TimeoutMs = 15 * 1000;

QUrl LatestReleaseUrl()
{
  // overridable for tests
  QString Url = qEnvironmentVariable("NIXSCP_UPDATE_URL");
  return QUrl(Url.isEmpty() ? QStringLiteral("https://api.github.com/repos/codewizdevs/nixscp/releases/latest") : Url);
}

QString PlainVersion(QString Version)
{
  Version = Version.trimmed();
  if (Version.startsWith(QLatin1Char('v'), Qt::CaseInsensitive))
  {
    Version.remove(0, 1);
  }
  return Version;
}

void Notify(QWidget * Window, const QString & Latest, const QUrl & Page)
{
  // not modal: it never blocks the work in NixSCP
  auto * Box = new QMessageBox(QMessageBox::Information, QStringLiteral("NixSCP"),
    QStringLiteral("<b>NixSCP %1 is available.</b>").arg(Latest.toHtmlEscaped()), QMessageBox::NoButton, Window);
  Box->setInformativeText(QStringLiteral("You are using version %1. NixSCP does not update itself: download the new "
    "version from its release page.").arg(QStringLiteral(PROJECT_VERSION)));
  Box->setAttribute(Qt::WA_DeleteOnClose);
  Box->setWindowModality(Qt::NonModal);
  QPushButton * Open = Box->addButton(QStringLiteral("Open download page"), QMessageBox::AcceptRole);
  QPushButton * Skip = Box->addButton(QStringLiteral("Skip this version"), QMessageBox::DestructiveRole);
  Box->addButton(QStringLiteral("Remind me later"), QMessageBox::RejectRole);
  Box->setDefaultButton(Open);
  QObject::connect(Box, &QMessageBox::finished, Box, [Box, Open, Skip, Latest, Page]()
  {
    if (Box->clickedButton() == Open)
    {
      OpenBrowser(Page);
    }
    else if (Box->clickedButton() == Skip)
    {
      Configuration & Config = Configuration::Instance();
      Config.UpdateSkipVersion = Latest;
      Config.Save();
    }
  });
  Box->show();
}

// Asks GitHub; Done gets the latest version and its page, or an error
void Fetch(QObject * Context, const std::function<void(const QString & Latest, const QUrl & Page, const QString & Error)> & Done)
{
  auto * Manager = new QNetworkAccessManager(Context);
  QNetworkRequest Request(LatestReleaseUrl());
  Request.setRawHeader("Accept", "application/vnd.github+json");
  Request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("NixSCP/%1").arg(QStringLiteral(PROJECT_VERSION)));
  Request.setTransferTimeout(TimeoutMs);
  QNetworkReply * Reply = Manager->get(Request);
  QObject::connect(Reply, &QNetworkReply::finished, Context, [Reply, Manager, Done]()
  {
    Reply->deleteLater();
    Manager->deleteLater();
    if (Reply->error() != QNetworkReply::NoError)
    {
      Done(QString(), QUrl(), Reply->errorString());
      return;
    }
    QJsonObject Release = QJsonDocument::fromJson(Reply->readAll()).object();
    QString Latest = PlainVersion(Release.value(QStringLiteral("tag_name")).toString());
    if (Latest.isEmpty() || Release.value(QStringLiteral("draft")).toBool() || Release.value(QStringLiteral("prerelease")).toBool())
    {
      Done(QString(), QUrl(), QStringLiteral("No release information."));
      return;
    }
    QUrl Page(Release.value(QStringLiteral("html_url")).toString());
    if (!Page.isValid() || (Page.scheme() != QStringLiteral("https")))
    {
      Page = QUrl(QStringLiteral("https://github.com/codewizdevs/nixscp/releases/latest"));
    }
    Done(Latest, Page, QString());
  });
}

}

QString CurrentVersion()
{
  return QStringLiteral(PROJECT_VERSION);
}

bool IsNewer(const QString & Latest, const QString & Current)
{
  QVersionNumber L = QVersionNumber::fromString(PlainVersion(Latest));
  QVersionNumber C = QVersionNumber::fromString(PlainVersion(Current));
  return !L.isNull() && (QVersionNumber::compare(L, C) > 0);
}

bool CheckDue(const QDateTime & LastCheck, const QDateTime & Now)
{
  if (!LastCheck.isValid())
  {
    return true;
  }
  qint64 Elapsed = LastCheck.secsTo(Now);
  return (Elapsed < 0) || (Elapsed >= CheckIntervalSecs);
}

void Start(QWidget * Window)
{
  Configuration & Config = Configuration::Instance();
  if (!Config.CheckForUpdates || qEnvironmentVariableIsSet("NIXSCP_NO_UPDATE_CHECK"))
  {
    return;
  }
  if (!CheckDue(Config.UpdateLastCheck, QDateTime::currentDateTimeUtc()))
  {
    return;
  }
  QPointer<QWidget> Guard(Window);
  bool DelayOk = false;
  int Delay = qEnvironmentVariableIntValue("NIXSCP_UPDATE_DELAY_MS", &DelayOk);  // tests
  QTimer::singleShot(DelayOk ? Delay : StartDelayMs, Window, [Guard]()
  {
    if (Guard == nullptr)
    {
      return;
    }
    Fetch(Guard, [Guard](const QString & Latest, const QUrl & Page, const QString & Error)
    {
      if (!Error.isEmpty() || (Guard == nullptr))
      {
        // offline or GitHub unreachable: try again on the next start
        return;
      }
      Configuration & Config = Configuration::Instance();
      Config.UpdateLastCheck = QDateTime::currentDateTimeUtc();
      Config.Save();
      if (IsNewer(Latest, QStringLiteral(PROJECT_VERSION)) && (Latest != Config.UpdateSkipVersion))
      {
        Notify(Guard, Latest, Page);
      }
    });
  });
}

void CheckNow(QWidget * Window)
{
  QPointer<QWidget> Guard(Window);
  Fetch(Window, [Guard](const QString & Latest, const QUrl & Page, const QString & Error)
  {
    if (Guard == nullptr)
    {
      return;
    }
    if (!Error.isEmpty())
    {
      QMessageBox::warning(Guard, QStringLiteral("NixSCP"), QStringLiteral("Cannot check for a new version: %1").arg(Error));
      return;
    }
    Configuration & Config = Configuration::Instance();
    Config.UpdateLastCheck = QDateTime::currentDateTimeUtc();
    Config.Save();
    if (IsNewer(Latest, QStringLiteral(PROJECT_VERSION)))
    {
      // asked for: shown even when this version was skipped
      Notify(Guard, Latest, Page);
    }
    else
    {
      QMessageBox::information(Guard, QStringLiteral("NixSCP"),
        QStringLiteral("You are using the latest version of NixSCP (%1).").arg(QStringLiteral(PROJECT_VERSION)));
    }
  });
}

}
