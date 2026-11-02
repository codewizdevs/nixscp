// HTTP client of the NixSCP Pro cloud API (nixscp-cloud api/, docs/SYNC.md "API").
// Bearer access tokens (15 minutes) are refreshed transparently with the
// rotating refresh token. Refreshing is single-flight: a refresh token works
// once, and presenting a used one signs the device out on the server.
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QUrl>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

class CloudClient : public QObject
{
  Q_OBJECT

public:
  struct Reply
  {
    int Status = 0;          // HTTP status, 0 = no response (network error, timeout)
    QJsonObject Json;        // parsed JSON object body (API calls)
    QByteArray Data;         // raw body
    QString Error;           // human readable, for failures
    bool Ok() const { return (Status >= 200) && (Status < 300); }
    QString ErrorCode() const { return Json.value(QStringLiteral("error")).toString(); }
  };
  using Callback = std::function<void(const Reply &)>;
  using Headers = QList<QPair<QByteArray, QByteArray>>;

  explicit CloudClient(QObject * Parent = nullptr);

  // https://nixscp.com, or $NIXSCP_CLOUD_URL (testing against another server)
  static QString DefaultBaseUrl();
  QString BaseUrl() const { return FBaseUrl; }
  void SetBaseUrl(const QString & Url);

  QString RefreshToken() const { return FRefreshToken; }
  void SetRefreshToken(const QString & Token);
  bool HasRefreshToken() const { return !FRefreshToken.isEmpty(); }

  // OAuth authorization code + PKCE exchange; stores the tokens on success.
  // Reply.Json has device_id.
  void ExchangeCode(const QString & Code, const QString & Verifier, const QString & RedirectUri, const Callback & Done);
  // Authenticated API call; Path like "/v1/sync/head?slot=settings"
  void Api(const QByteArray & Method, const QString & Path, const QJsonObject & Body, const Callback & Done);
  void Api(const QByteArray & Method, const QString & Path, const Callback & Done) { Api(Method, Path, QJsonObject(), Done); }
  // Signed R2 URL (no API authentication)
  void Transfer(const QByteArray & Method, const QUrl & Url, const QByteArray & Data, const Headers & RequestHeaders, const Callback & Done);
  // Forgets the tokens (after revoking them on the server, when possible)
  void SignOut(const std::function<void()> & Done = nullptr);

signals:
  // The refresh token rotated: persist RefreshToken()
  void TokensChanged();
  // The server rejected the refresh token. Reason (when the server knows it):
  // "password_changed", "password_reset", "removed", "token_reused", "signed_out"
  void SignedOut(const QString & Reason);

private:
  QNetworkAccessManager * FManager;
  QString FBaseUrl;
  QString FAccessToken;
  QString FRefreshToken;
  bool FRefreshing = false;
  QList<std::function<void(bool)>> FWaitingForRefresh;

  void Send(const QByteArray & Method, const QUrl & Url, const QByteArray & Body, const Headers & RequestHeaders,
    const Callback & Done);
  void SendApi(const QByteArray & Method, const QString & Path, const QJsonObject & Body, bool MayRefresh, const Callback & Done);
  void Refresh(const std::function<void(bool)> & Done);
  void SetTokens(const QJsonObject & Json);
};
