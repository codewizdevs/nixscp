// NixSCP Pro on the phone: the account (sign-in in the browser, back to the
// app through a loopback callback, as on desktop), the sync password that
// unlocks the data key (first device: created, with an optional recovery
// key), settings sync (core/CloudSync: the sites and passwords, end-to-end
// encrypted; the phone's open tabs are not synced), conflicts, the license.
#pragma once

#include <QObject>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QVariantMap>

class AndroidFiles;

class ProAccount : public QObject
{
  Q_OBJECT
  // disabled, signedOut, needKey, idle, syncing, conflict, paused, noLicense, error
  Q_PROPERTY(QString state READ state NOTIFY changed)
  Q_PROPERTY(QString statusText READ statusText NOTIFY changed)
  Q_PROPERTY(QString email READ email NOTIFY changed)
  Q_PROPERTY(bool signedIn READ signedIn NOTIFY changed)
  Q_PROPERTY(bool hasKey READ hasKey NOTIFY changed)
  Q_PROPERTY(QString lastSync READ lastSync NOTIFY changed)
  Q_PROPERTY(bool signingIn READ signingIn NOTIFY changed)
  // the page of the running sign-in (to copy when no browser opened)
  Q_PROPERTY(QString signInUrl READ signInUrl NOTIFY changed)
  Q_PROPERTY(QString accountUrl READ accountUrl CONSTANT)
  Q_PROPERTY(QString registerUrl READ registerUrl CONSTANT)

public:
  explicit ProAccount(AndroidFiles * Files, QObject * Parent = nullptr);

  // after the configuration is loaded (CloudSync and TeamSync are initialized)
  void Start();
  QString state() const;
  QString statusText() const;
  QString email() const;
  bool signedIn() const;
  bool hasKey() const;
  QString lastSync() const;
  bool signingIn() const { return FServer != nullptr; }
  QString signInUrl() const { return signingIn() ? FUrl : QString(); }
  QString accountUrl() const;
  QString registerUrl() const;

  // opens the sign-in page in the browser (signInFinished)
  Q_INVOKABLE void signIn();
  Q_INVOKABLE void cancelSignIn();
  Q_INVOKABLE void signOut();
  // whether the cloud has data (probed): unlock it, or create the key
  Q_INVOKABLE void probe();
  // keyFinished(ok, recovery key or error)
  Q_INVOKABLE void createKey(const QString & Password, bool WithRecoveryKey);
  // keyFinished(ok, error)
  Q_INVOKABLE void unlockKey(const QString & Secret, bool IsRecoveryKey);
  Q_INVOKABLE void changePassword(const QString & Password);
  Q_INVOKABLE void syncNow();
  // 0 = the cloud's, 1 = this phone's, 2 = later
  Q_INVOKABLE void resolveConflict(int Choice);
  // account(map: email, license {active, plan, status, beta...})
  Q_INVOKABLE void fetchAccount();
  // the app is in front again: what changed elsewhere
  Q_INVOKABLE void foreground();

signals:
  void changed();
  void signInFinished(bool ok, const QString & error);
  void probed(bool ok, bool hasData, const QString & error);
  void keyFinished(bool ok, const QString & result);
  void passwordChanged(bool ok, const QString & error);
  void account(const QVariantMap & data);
  void conflict(const QString & device, const QString & time);
  // something to show (settings pulled, signed out by the server...)
  void notice(const QString & message);
  void licenseNotice(bool betaEnded);
  // the sites may have changed (pulled settings, teams)
  void sitesChanged();

private:
  AndroidFiles * FFiles;
  QPointer<QTcpServer> FServer;
  QTimer FSignInTimeout;
  QTimer FLicenseTimer;
  QTimer FTeamsTimer;
  // the sign-in request (CloudSync::SignInRequest)
  QString FUrl;
  QString FVerifier;
  QString FState;
  QString FRedirectUri;

  void Accept();
  void Handle(QTcpSocket * Socket, const QByteArray & RequestLine);
  void Respond(QTcpSocket * Socket, int Status, const QString & Message, bool Back);
  void FinishSignIn(bool Ok, const QString & Error);
};
