#include "mobile/ProAccount.h"
#include "mobile/AndroidFiles.h"

#include "core/CloudSync.h"
#include "core/TeamSync.h"

#include <QDesktopServices>
#include <QJsonObject>
#include <QLocale>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>

namespace
{
CloudSync & Sync() { return CloudSync::Instance(); }
}

ProAccount::ProAccount(AndroidFiles * Files, QObject * Parent) : QObject(Parent), FFiles(Files)
{
  FSignInTimeout.setSingleShot(true);
  FSignInTimeout.setInterval(10 * 60 * 1000);
  connect(&FSignInTimeout, &QTimer::timeout, this, [this]() { FinishSignIn(false, QStringLiteral("Sign-in timed out.")); });
  // the license daily, teams every few minutes (as desktop)
  FLicenseTimer.setInterval(24 * 60 * 60 * 1000);
  connect(&FLicenseTimer, &QTimer::timeout, this, []() { Sync().CheckLicense(); });
  FTeamsTimer.setInterval(3 * 60 * 1000);
  connect(&FTeamsTimer, &QTimer::timeout, this, []()
  {
    if (TeamSync::Instance().Available())
    {
      TeamSync::Instance().SyncAll();
    }
  });
}

void ProAccount::Start()
{
  CloudSync & S = Sync();
  // the phone's own tabs stay out of the cloud (and the desktop's are not reopened)
  S.SetSyncSessions(false);
  S.SetRestoreSessions(false);
  connect(&S, &CloudSync::StateChanged, this, &ProAccount::changed);
  connect(&S, &CloudSync::SettingsPulled, this, [this](const QString & Device)
  {
    emit sitesChanged();
    emit notice(Device.isEmpty() ? QStringLiteral("Settings synced from the cloud.")
                                 : QStringLiteral("Settings synced from %1.").arg(Device));
  });
  connect(&S, &CloudSync::ConflictDetected, this, [this](const QString & Device, const QDateTime & Time)
  {
    QTimer::singleShot(0, this, [this, Device, Time]()
    {
      emit conflict(Device, QLocale().toString(Time.toLocalTime(), QLocale::ShortFormat));
    });
  });
  connect(&S, &CloudSync::SignedOutRemotely, this, [this](const QString & Message) { emit notice(Message); emit changed(); });
  connect(&S, &CloudSync::LicenseInactive, this, [this]()
  {
    Sync().FetchAccount([this](bool Ok, const QJsonObject & Me)
    {
      emit licenseNotice(Ok && Me.value(QStringLiteral("license")).toObject().value(QStringLiteral("beta_ended")).toBool());
    });
  });
  connect(&S, &CloudSync::ForeignSnapshotIgnored, this, [this](const QString & Device)
  {
    emit notice(QStringLiteral("Settings uploaded by %1 could not be decrypted with your NixSCP password and were "
      "replaced. Enter your NixSCP password on %1 to sync it again.").arg(Device));
  });
  connect(&TeamSync::Instance(), &TeamSync::Changed, this, &ProAccount::sitesChanged);
  connect(&TeamSync::Instance(), &TeamSync::Notice, this, &ProAccount::notice);
  FLicenseTimer.start();
  FTeamsTimer.start();
  QTimer::singleShot(30 * 1000, this, []() { Sync().CheckLicense(); });
  QTimer::singleShot(20 * 1000, this, []()
  {
    if (TeamSync::Instance().Available())
    {
      TeamSync::Instance().SyncAll();
    }
  });
}

QString ProAccount::state() const
{
  switch (Sync().State())
  {
    case CloudSync::csDisabled: return QStringLiteral("disabled");
    case CloudSync::csSignedOut: return QStringLiteral("signedOut");
    case CloudSync::csNeedKey: return QStringLiteral("needKey");
    case CloudSync::csIdle: return QStringLiteral("idle");
    case CloudSync::csSyncing: return QStringLiteral("syncing");
    case CloudSync::csConflict: return QStringLiteral("conflict");
    case CloudSync::csPaused: return QStringLiteral("paused");
    case CloudSync::csNoLicense: return QStringLiteral("noLicense");
    case CloudSync::csError: return QStringLiteral("error");
  }
  return QString();
}

QString ProAccount::statusText() const { return Sync().StatusText(); }
QString ProAccount::email() const { return Sync().Email(); }
bool ProAccount::signedIn() const { return Sync().SignedIn(); }
bool ProAccount::hasKey() const { return Sync().HasKey(); }

QString ProAccount::lastSync() const
{
  QDateTime Time = Sync().LastSync();
  return Time.isValid() ? QLocale().toString(Time.toLocalTime(), QLocale::ShortFormat) : QString();
}

QString ProAccount::accountUrl() const { return Sync().Client().BaseUrl() + QStringLiteral("/account"); }
QString ProAccount::registerUrl() const { return Sync().Client().BaseUrl() + QStringLiteral("/login?register&next=/account"); }

//---------------------------------------------------------------------------
// sign-in: the browser comes back to http://127.0.0.1:<port>/callback (the
// app keeps running meanwhile: the transfer service)
void ProAccount::signIn()
{
  cancelSignIn();
  auto * Server = new QTcpServer(this);
  if (!Server->listen(QHostAddress::LocalHost, 0))
  {
    delete Server;
    emit signInFinished(false, QStringLiteral("Cannot start the sign-in."));
    return;
  }
  FServer = Server;
  connect(Server, &QTcpServer::newConnection, this, &ProAccount::Accept);
  CloudSync::SignInRequest Request = Sync().NewSignInRequest(Server->serverPort());
  FUrl = Request.Url;
  FVerifier = Request.Verifier;
  FState = Request.State;
  FRedirectUri = Request.RedirectUri;
  FSignInTimeout.start();
  FFiles->keepAlive(true, QStringLiteral("Signing in to NixSCP Pro..."));
  emit changed();
  QDesktopServices::openUrl(QUrl(FUrl, QUrl::StrictMode));
}

void ProAccount::cancelSignIn()
{
  if (FServer != nullptr)
  {
    FServer->close();
    FServer->deleteLater();
    FServer = nullptr;
    FSignInTimeout.stop();
    FFiles->keepAlive(false, QString());
    emit changed();
  }
}

void ProAccount::FinishSignIn(bool Ok, const QString & Error)
{
  cancelSignIn();
  emit signInFinished(Ok, Error);
  emit changed();
}

void ProAccount::Accept()
{
  while ((FServer != nullptr) && FServer->hasPendingConnections())
  {
    QTcpSocket * Socket = FServer->nextPendingConnection();
    connect(Socket, &QTcpSocket::disconnected, Socket, &QObject::deleteLater);
    connect(Socket, &QTcpSocket::readyRead, this, [this, Socket]()
    {
      QByteArray Data = Socket->peek(8192);
      if (!Data.contains("\r\n\r\n") && (Data.size() < 8192))
      {
        return;
      }
      Socket->readAll();
      Handle(Socket, Data.left(Data.indexOf("\r\n")));
    });
  }
}

void ProAccount::Respond(QTcpSocket * Socket, int Status, const QString & Message, bool Back)
{
  // a button back to the app (Android opens it by its package)
  QString Link = Back ? QStringLiteral("<p><a href=\"intent:#Intent;action=android.intent.action.MAIN;"
    "category=android.intent.category.LAUNCHER;package=com.nixscp.android;end\" style=\"display:inline-block;"
    "padding:.8rem 1.4rem;background:#1d4ed8;color:#fff;border-radius:8px;text-decoration:none\">Back to NixSCP</a></p>")
    : QString();
  QByteArray Body = QStringLiteral(
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width\">"
    "<title>NixSCP</title></head><body style=\"font-family:system-ui,sans-serif;max-width:32rem;margin:3rem auto;"
    "padding:0 1rem\"><h1>NixSCP</h1><p>%1</p>%2</body></html>").arg(Message.toHtmlEscaped(), Link).toUtf8();
  QByteArray Response = QByteArray("HTTP/1.1 ") + QByteArray::number(Status) + ((Status == 200) ? " OK" : " Bad Request") +
    "\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: " +
    QByteArray::number(Body.size()) + "\r\n\r\n" + Body;
  Socket->write(Response);
  Socket->disconnectFromHost();
}

void ProAccount::Handle(QTcpSocket * Socket, const QByteArray & RequestLine)
{
  // "GET /callback?code=...&state=... HTTP/1.1"
  QList<QByteArray> Parts = RequestLine.split(' ');
  QUrl Url((Parts.size() >= 2) ? QString::fromLatin1(Parts[1]) : QString());
  if ((Parts.value(0) != "GET") || (Url.path() != QStringLiteral("/callback")))
  {
    Respond(Socket, 400, QStringLiteral("Unexpected request."), false);
    return;
  }
  QUrlQuery Query(Url);
  // the state ties the answer to this sign-in (no forged callbacks)
  if (Query.queryItemValue(QStringLiteral("state")) != FState)
  {
    Respond(Socket, 400, QStringLiteral("This sign-in does not match the one NixSCP started. Start again from NixSCP."), false);
    return;
  }
  if (!Query.queryItemValue(QStringLiteral("error")).isEmpty())
  {
    Respond(Socket, 200, QStringLiteral("Sign-in was cancelled."), true);
    FinishSignIn(false, QStringLiteral("Sign-in was cancelled in the browser."));
    return;
  }
  QString Code = Query.queryItemValue(QStringLiteral("code"));
  Respond(Socket, 200, QStringLiteral("NixSCP is signed in. Go back to NixSCP to finish."), true);
  CloudSync::SignInRequest Request;
  Request.Url = FUrl;
  Request.Verifier = FVerifier;
  Request.State = FState;
  Request.RedirectUri = FRedirectUri;
  Sync().CompleteSignIn(Request, Code, [this](bool Ok, const QString & Error) { FinishSignIn(Ok, Error); });
}

void ProAccount::signOut()
{
  Sync().SignOut();
  emit changed();
}

//---------------------------------------------------------------------------
void ProAccount::probe()
{
  Sync().ProbeCloud([this](bool Ok, bool HasData, const QString & Error) { emit probed(Ok, HasData, Error); });
}

void ProAccount::createKey(const QString & Password, bool WithRecoveryKey)
{
  Sync().CreateKey(Password, WithRecoveryKey, [this](bool Ok, const QString & Result)
  {
    emit keyFinished(Ok, Result);
    emit changed();
  });
}

void ProAccount::unlockKey(const QString & Secret, bool IsRecoveryKey)
{
  Sync().UnlockKey(Secret, IsRecoveryKey, [this](bool Ok, const QString & Error)
  {
    emit keyFinished(Ok, Error);
    emit changed();
    if (Ok)
    {
      emit sitesChanged();
    }
  });
}

void ProAccount::changePassword(const QString & Password)
{
  Sync().ChangePassword(Password, [this](bool Ok, const QString & Error) { emit passwordChanged(Ok, Error); });
}

void ProAccount::syncNow()
{
  Sync().SyncNow();
  if (TeamSync::Instance().Available())
  {
    TeamSync::Instance().SyncAll();
  }
}

void ProAccount::resolveConflict(int Choice)
{
  Sync().ResolveConflict((Choice == 0) ? CloudSync::crUseCloud : ((Choice == 1) ? CloudSync::crUseLocal : CloudSync::crLater));
}

void ProAccount::fetchAccount()
{
  Sync().FetchAccount([this](bool Ok, const QJsonObject & Me)
  {
    emit account(Ok ? Me.toVariantMap() : QVariantMap{{QStringLiteral("error"), true}});
  });
}

void ProAccount::foreground()
{
  if (Sync().SignedIn())
  {
    Sync().SyncIfStale(60);
  }
}
