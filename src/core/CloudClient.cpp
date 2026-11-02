#include "core/CloudClient.h"

#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

#include <utility>

static constexpr int TransferTimeoutMs = 30000;

CloudClient::CloudClient(QObject * Parent) :
  QObject(Parent),
  FManager(new QNetworkAccessManager(this)),
  FBaseUrl(DefaultBaseUrl())
{
}

QString CloudClient::DefaultBaseUrl()
{
  QString Url = qEnvironmentVariable("NIXSCP_CLOUD_URL");
  if (Url.isEmpty())
  {
    Url = QStringLiteral("https://nixscp.com");
  }
  while (Url.endsWith(QLatin1Char('/')))
  {
    Url.chop(1);
  }
  return Url;
}

void CloudClient::SetBaseUrl(const QString & Url)
{
  FBaseUrl = Url;
  while (FBaseUrl.endsWith(QLatin1Char('/')))
  {
    FBaseUrl.chop(1);
  }
}

void CloudClient::SetRefreshToken(const QString & Token)
{
  FRefreshToken = Token;
  FAccessToken.clear();
}

void CloudClient::Send(const QByteArray & Method, const QUrl & Url, const QByteArray & Body, const Headers & RequestHeaders,
  const Callback & Done)
{
  QNetworkRequest Request(Url);
  Request.setTransferTimeout(TransferTimeoutMs);
  Request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("NixSCP"));
  // Signed URLs and the API never redirect; do not follow anything elsewhere
  Request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
  for (const auto & Header : RequestHeaders)
  {
    Request.setRawHeader(Header.first, Header.second);
  }
  QNetworkReply * NetReply = FManager->sendCustomRequest(Request, Method, Body);
  connect(NetReply, &QNetworkReply::finished, this, [NetReply, Done]()
  {
    NetReply->deleteLater();
    Reply Result;
    Result.Status = NetReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    Result.Data = NetReply->readAll();
    QJsonParseError ParseError;
    QJsonDocument Doc = QJsonDocument::fromJson(Result.Data, &ParseError);
    if (Doc.isObject())
    {
      Result.Json = Doc.object();
    }
    if (Result.Status == 0)
    {
      Result.Error = QStringLiteral("Cannot connect to the NixSCP cloud: %1").arg(NetReply->errorString());
    }
    else if (!Result.Ok())
    {
      QString Code = Result.ErrorCode();
      Result.Error = QStringLiteral("The NixSCP cloud returned an error (%1%2).")
        .arg(Result.Status).arg(Code.isEmpty() ? QString() : QStringLiteral(", ") + Code);
    }
    Done(Result);
  });
}

void CloudClient::SetTokens(const QJsonObject & Json)
{
  FAccessToken = Json.value(QStringLiteral("access_token")).toString();
  FRefreshToken = Json.value(QStringLiteral("refresh_token")).toString();
  emit TokensChanged();
}

void CloudClient::ExchangeCode(const QString & Code, const QString & Verifier, const QString & RedirectUri, const Callback & Done)
{
  QJsonObject Body
  {
    {QStringLiteral("grant_type"), QStringLiteral("authorization_code")},
    {QStringLiteral("code"), Code},
    {QStringLiteral("code_verifier"), Verifier},
    {QStringLiteral("redirect_uri"), RedirectUri},
  };
  Send("POST", QUrl(FBaseUrl + QStringLiteral("/v1/oauth/token")), QJsonDocument(Body).toJson(QJsonDocument::Compact),
    {{"Content-Type", "application/json"}}, [this, Done](const Reply & Result)
  {
    if (Result.Ok() && !Result.Json.value(QStringLiteral("refresh_token")).toString().isEmpty())
    {
      SetTokens(Result.Json);
    }
    Done(Result);
  });
}

void CloudClient::Refresh(const std::function<void(bool)> & Done)
{
  FWaitingForRefresh.append(Done);
  if (FRefreshing)
  {
    return;
  }
  if (FRefreshToken.isEmpty())
  {
    auto Waiting = std::exchange(FWaitingForRefresh, {});
    for (const auto & Callback : Waiting)
    {
      Callback(false);
    }
    return;
  }
  FRefreshing = true;
  QJsonObject Body
  {
    {QStringLiteral("grant_type"), QStringLiteral("refresh_token")},
    {QStringLiteral("refresh_token"), FRefreshToken},
  };
  Send("POST", QUrl(FBaseUrl + QStringLiteral("/v1/oauth/token")), QJsonDocument(Body).toJson(QJsonDocument::Compact),
    {{"Content-Type", "application/json"}}, [this](const Reply & Result)
  {
    FRefreshing = false;
    bool Ok = Result.Ok() && !Result.Json.value(QStringLiteral("refresh_token")).toString().isEmpty();
    if (Ok)
    {
      SetTokens(Result.Json);
    }
    else if ((Result.Status == 400) || (Result.Status == 401))
    {
      // The token is invalid (revoked from the website, or replayed): signed out
      FAccessToken.clear();
      FRefreshToken.clear();
      emit TokensChanged();
      emit SignedOut(Result.Json.value(QStringLiteral("reason")).toString());
    }
    auto Waiting = std::exchange(FWaitingForRefresh, {});
    for (const auto & Callback : Waiting)
    {
      Callback(Ok);
    }
  });
}

void CloudClient::Api(const QByteArray & Method, const QString & Path, const QJsonObject & Body, const Callback & Done)
{
  if (FAccessToken.isEmpty())
  {
    Refresh([this, Method, Path, Body, Done](bool Ok)
    {
      if (!Ok)
      {
        Reply Result;
        Result.Status = FRefreshToken.isEmpty() ? 401 : 0;
        Result.Error = FRefreshToken.isEmpty() ? QStringLiteral("You are signed out of the NixSCP cloud.")
                                               : QStringLiteral("Cannot connect to the NixSCP cloud.");
        Done(Result);
        return;
      }
      SendApi(Method, Path, Body, false, Done);
    });
    return;
  }
  SendApi(Method, Path, Body, true, Done);
}

void CloudClient::SendApi(const QByteArray & Method, const QString & Path, const QJsonObject & Body, bool MayRefresh, const Callback & Done)
{
  Headers RequestHeaders{{"Authorization", "Bearer " + FAccessToken.toLatin1()}};
  QByteArray Data;
  if (Method != "GET")
  {
    RequestHeaders.append(qMakePair(QByteArray("Content-Type"), QByteArray("application/json")));
    Data = QJsonDocument(Body).toJson(QJsonDocument::Compact);
  }
  Send(Method, QUrl(FBaseUrl + Path), Data, RequestHeaders, [this, Method, Path, Body, MayRefresh, Done](const Reply & Result)
  {
    if ((Result.Status == 401) && MayRefresh)
    {
      // The access token expired: refresh once and retry
      FAccessToken.clear();
      Api(Method, Path, Body, Done);
      return;
    }
    Done(Result);
  });
}

void CloudClient::Transfer(const QByteArray & Method, const QUrl & Url, const QByteArray & Data, const Headers & RequestHeaders,
  const Callback & Done)
{
  Send(Method, Url, Data, RequestHeaders, Done);
}

void CloudClient::SignOut(const std::function<void()> & Done)
{
  auto Forget = [this, Done]()
  {
    FAccessToken.clear();
    FRefreshToken.clear();
    emit TokensChanged();
    if (Done)
    {
      Done();
    }
  };
  if (FRefreshToken.isEmpty())
  {
    Forget();
    return;
  }
  Api("POST", QStringLiteral("/v1/oauth/revoke"), [Forget](const Reply &) { Forget(); });
}
