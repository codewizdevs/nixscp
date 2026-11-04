#include "gui/CloudSyncUi.h"
#include "gui/UnlockDialog.h"

#include "core/TeamSync.h"
#include "core/ConfigVault.h"
#include "core/AgentAccess.h"
#include "core/CloudSync.h"
#include "core/Configuration.h"
#include "core/IniStorage.h"
#include "core/Workspaces.h"
#include "gui/CommanderWindow.h"
#include "gui/GuiUserInterface.h"

#include <QCheckBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QSaveFile>
#include <QTextCursor>
#include <QFontDatabase>
#include <QToolButton>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

namespace CloudSyncUi
{

// Name of the scratch workspace the open tabs are saved as; it only exists in
// the session payload, never in the user's configuration
static const QString CloudWorkspace = QStringLiteral("NixSCP Cloud Session");
// Open tabs are uploaded at most once a minute while they change
static constexpr int SessionIntervalMs = 60 * 1000;
static constexpr int LicenseFirstCheckMs = 30 * 1000;
static constexpr int TeamsFirstSyncMs = 20 * 1000;
static constexpr int TeamsIntervalMs = 60 * 1000;
static constexpr int LicenseCheckIntervalMs = 24 * 60 * 60 * 1000;
// Focusing the window checks the cloud when the last check is older
static constexpr int FocusCheckSeconds = 60;
static constexpr int FlushTimeoutMs = 4000;
static constexpr int RestoreTimeoutMs = 5000;
static constexpr int MinSyncPasswordLength = 8;

//---------------------------------------------------------------------------
// Session payload

static QString WorkspacePrefix()
{
  return QStringLiteral("Sessions\\") + MungeStr(CloudWorkspace + QLatin1Char('/'), false);
}

// Independent copy of the stored sites (the workspace functions modify the list)
static StoredSessionList CopyOfSites(IniFile & Sites)
{
  Configuration::Instance().StoredSessions().Save(Sites);
  StoredSessionList Copy;
  Copy.Load(Sites);
  return Copy;
}

QByteArray SessionPayload(const QList<SessionData> & Tabs)
{
  if (Tabs.isEmpty())
  {
    return QByteArray();
  }
  IniFile Sites;
  StoredSessionList Scratch = CopyOfSites(Sites);
  Workspaces::SaveWorkspace(Scratch, CloudWorkspace, Tabs, Configuration::Instance().AutoSaveWorkspacePasswords);
  IniFile Saved;
  Scratch.Save(Saved);
  IniFile Result;
  const QString Prefix = WorkspacePrefix();
  for (const QString & Section : Saved.Sections())
  {
    if (Section.startsWith(Prefix, Qt::CaseInsensitive))
    {
      Result.AddSection(Section);
      for (const QString & Key : Saved.Keys(Section))
      {
        Result.WriteRaw(Section, Key, Saved.ReadRaw(Section, Key));
      }
    }
  }
  return Result.SaveToData();
}

QList<SessionData> SessionsFromPayload(const QByteArray & Payload)
{
  IniFile Tabs;
  if (Payload.trimmed().isEmpty() || !Tabs.LoadFromData(Payload))
  {
    return QList<SessionData>();
  }
  IniFile Sites;
  Configuration::Instance().StoredSessions().Save(Sites);
  // Only the workspace sections of the payload, never anything else
  const QString Prefix = WorkspacePrefix();
  for (const QString & Section : Tabs.Sections())
  {
    if (Section.startsWith(Prefix, Qt::CaseInsensitive))
    {
      Sites.EraseSection(Section);
      Sites.AddSection(Section);
      for (const QString & Key : Tabs.Keys(Section))
      {
        Sites.WriteRaw(Section, Key, Tabs.ReadRaw(Section, Key));
      }
    }
  }
  StoredSessionList Scratch;
  Scratch.Load(Sites);
  QList<SessionData> Result = Workspaces::FolderOrWorkspace(Scratch, CloudWorkspace);
  for (SessionData & Data : Result)
  {
    // Local directories of another computer usually do not exist here
    if (!Data.LocalDirectory.isEmpty() && !QDir(Data.LocalDirectory).exists())
    {
      Data.LocalDirectory.clear();
    }
    if (!Data.OtherLocalDirectory.isEmpty() && !QDir(Data.OtherLocalDirectory).exists())
    {
      Data.OtherLocalDirectory.clear();
    }
    Data.IsWorkspace = false;
  }
  return Result;
}


void RestoreSessions(CommanderWindow * Window, const std::function<void(bool)> & Done)
{
  QPointer<CommanderWindow> Guard(Window);
  CloudSync::Instance().FetchSession(RestoreTimeoutMs, [Guard, Done](bool Found, const QByteArray & Payload)
  {
    if (!Guard)
    {
      return;
    }
    QList<SessionData> Sessions = Found ? SessionsFromPayload(Payload) : QList<SessionData>();
    for (const SessionData & Data : Sessions)
    {
      Guard->OpenSession(Data);
    }
    Done(!Sessions.isEmpty());
  });
}

//---------------------------------------------------------------------------
// Browser sign-in: OAuth authorization code + PKCE with a loopback redirect

class LoopbackSignIn : public QObject
{
public:
  using Finished = std::function<void(bool Ok, const QString & Error)>;

  LoopbackSignIn(QObject * Parent, Finished Done) : QObject(Parent), FDone(std::move(Done))
  {
    FServer = new QTcpServer(this);
    connect(FServer, &QTcpServer::newConnection, this, &LoopbackSignIn::Accept);
    FTimeout = new QTimer(this);
    FTimeout->setSingleShot(true);
    connect(FTimeout, &QTimer::timeout, this, [this]() { Finish(false, QStringLiteral("Sign-in timed out.")); });
  }

  bool Start()
  {
    if (!FServer->listen(QHostAddress::LocalHost, 0))
    {
      return false;
    }
    FRequest = CloudSync::Instance().NewSignInRequest(FServer->serverPort());
    FTimeout->start(10 * 60 * 1000);
    return QDesktopServices::openUrl(QUrl(FRequest.Url, QUrl::StrictMode));
  }

  QString Url() const { return FRequest.Url; }

private:
  QTcpServer * FServer;
  QTimer * FTimeout;
  CloudSync::SignInRequest FRequest;
  Finished FDone;
  bool FFinished = false;

  void Finish(bool Ok, const QString & Error)
  {
    if (!FFinished)
    {
      FFinished = true;
      FServer->close();
      FDone(Ok, Error);
    }
  }

  void Accept()
  {
    while (QTcpSocket * Socket = FServer->nextPendingConnection())
    {
      connect(Socket, &QTcpSocket::disconnected, Socket, &QObject::deleteLater);
      connect(Socket, &QTcpSocket::readyRead, this, [this, Socket]()
      {
        QByteArray Data = Socket->peek(8192);
        if (!Data.contains("\r\n\r\n") && (Data.size() < 8192))
        {
          return;
        }
        Socket->readAll();
        HandleRequest(Socket, Data.left(Data.indexOf("\r\n")));
      });
    }
  }

  void Respond(QTcpSocket * Socket, int Status, const QString & Message)
  {
    QByteArray Body = QStringLiteral(
      "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>NixSCP</title></head>"
      "<body style=\"font-family:system-ui,sans-serif;max-width:32rem;margin:4rem auto;padding:0 1rem\">"
      "<h1>NixSCP</h1><p>%1</p></body></html>").arg(Message.toHtmlEscaped()).toUtf8();
    QByteArray Response = QByteArray("HTTP/1.1 ") + QByteArray::number(Status) + ((Status == 200) ? " OK" : " Bad Request") +
      "\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: " +
      QByteArray::number(Body.size()) + "\r\n\r\n" + Body;
    Socket->write(Response);
    Socket->disconnectFromHost();
  }

  void HandleRequest(QTcpSocket * Socket, const QByteArray & RequestLine)
  {
    // "GET /callback?code=...&state=... HTTP/1.1"
    QList<QByteArray> Parts = RequestLine.split(' ');
    QUrl Url((Parts.size() >= 2) ? QString::fromLatin1(Parts[1]) : QString());
    if ((Parts.value(0) != "GET") || (Url.path() != QStringLiteral("/callback")))
    {
      Respond(Socket, 400, QStringLiteral("Unexpected request."));
      return;
    }
    QUrlQuery Query(Url);
    // The state ties the answer to this very sign-in (no forged callbacks)
    if (Query.queryItemValue(QStringLiteral("state")) != FRequest.State)
    {
      Respond(Socket, 400, QStringLiteral("This sign-in does not match the one NixSCP started. Start again from NixSCP."));
      return;
    }
    if (!Query.queryItemValue(QStringLiteral("error")).isEmpty())
    {
      Respond(Socket, 200, QStringLiteral("Sign-in was cancelled. You can close this tab."));
      Finish(false, QStringLiteral("Sign-in was cancelled in the browser."));
      return;
    }
    QString Code = Query.queryItemValue(QStringLiteral("code"));
    Respond(Socket, 200, QStringLiteral("NixSCP is signed in. You can close this tab and return to NixSCP."));
    CloudSync::Instance().CompleteSignIn(FRequest, Code, [this](bool Ok, const QString & Error) { Finish(Ok, Error); });
  }
};

//---------------------------------------------------------------------------
// Options > Cloud Sync...

class CloudSyncDialog : public QDialog
{
public:
  explicit CloudSyncDialog(QWidget * Parent) : QDialog(Parent)
  {
    setWindowTitle(QStringLiteral("Cloud Sync"));
    setMinimumWidth(520);
    auto * Layout = new QVBoxLayout(this);
    FPages = new QStackedWidget(this);
    Layout->addWidget(FPages);
    FPages->addWidget(CreateSignedOutPage());
    FPages->addWidget(CreateKeyPage());
    FPages->addWidget(CreateReadyPage());
    auto * Buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    Layout->addWidget(Buttons);
    connect(&CloudSync::Instance(), &CloudSync::StateChanged, this, [this]() { Update(); });
    Update();
  }

private:
  QStackedWidget * FPages;
  // Signed out
  QLabel * FSignedOutStatus;
  QPushButton * FSignInButton;
  QPointer<LoopbackSignIn> FSignIn;
  // Key
  QLabel * FKeyIntro;
  QLineEdit * FPassword;
  QLineEdit * FPasswordConfirm;
  QLabel * FConfirmLabel;
  QCheckBox * FRecoveryCheck;
  QPushButton * FKeyButton;
  QToolButton * FUseRecoveryKey;
  QLabel * FKeyError;
  bool FCloudHasData = false;
  bool FProbed = false;
  bool FRecoveryMode = false;
  // Ready
  QLabel * FAccount;
  QLabel * FLicense;
  QLabel * FStatus;
  QCheckBox * FSyncSessions;
  QCheckBox * FRestoreSessions;
  QPushButton * FRemoveEncryption;
  int FShownPage = -1;

  static QLabel * Text(const QString & Html, QWidget * Parent)
  {
    auto * Label = new QLabel(Html, Parent);
    Label->setWordWrap(true);
    Label->setTextFormat(Qt::RichText);
    Label->setOpenExternalLinks(true);
    return Label;
  }

  QWidget * CreateSignedOutPage()
  {
    auto * Page = new QWidget(this);
    auto * Layout = new QVBoxLayout(Page);
    Layout->setContentsMargins(0, 0, 0, 0);
    Layout->addWidget(Text(QStringLiteral(
      "<h3>NixSCP Pro: your sites on every computer</h3>"
      "<p>Sync your sites, preferences, workspaces and open sessions between computers. "
      "Everything is encrypted on this computer with your own sync password before it is uploaded; "
      "the NixSCP cloud only ever stores encrypted data.</p>"), Page));
    FSignedOutStatus = Text(QString(), Page);
    Layout->addWidget(FSignedOutStatus);
    auto * Row = new QHBoxLayout();
    FSignInButton = new QPushButton(QStringLiteral("Sign in with browser..."), Page);
    auto * Create = new QPushButton(QStringLiteral("Create account..."), Page);
    Row->addWidget(FSignInButton);
    Row->addWidget(Create);
    Row->addStretch();
    Layout->addLayout(Row);
    // Signed out, the configuration stays encrypted with the NixSCP password until removed here
    FRemoveEncryption = new QPushButton(QStringLiteral("Remove encryption from this computer..."), Page);
    auto * RemoveRow = new QHBoxLayout();
    RemoveRow->addWidget(FRemoveEncryption);
    RemoveRow->addStretch();
    Layout->addLayout(RemoveRow);
    Layout->addStretch();
    connect(FRemoveEncryption, &QPushButton::clicked, this, [this]()
    {
      if (QMessageBox::question(this, QStringLiteral("Remove encryption"),
            QStringLiteral("Store the configuration on this computer unencrypted?\n\nNixSCP will no longer ask for your NixSCP "
              "password at startup, and saved site passwords are then only obfuscated, as in WinSCP. Signing in to NixSCP Pro "
              "encrypts it again.")) != QMessageBox::Yes)
      {
        return;
      }
      ConfigVault::Disable();
      if (!Configuration::Instance().Rewrite())
      {
        QMessageBox::warning(this, QStringLiteral("Remove encryption"), Configuration::Instance().LastSaveError());
      }
      Update();
    });
    connect(FSignInButton, &QPushButton::clicked, this, [this]() { SignIn(); });
    connect(Create, &QPushButton::clicked, this, []()
    {
      QDesktopServices::openUrl(QUrl(CloudSync::Instance().Client().BaseUrl() + QStringLiteral("/login?register&next=/account")));
    });
    return Page;
  }

  QWidget * CreateKeyPage()
  {
    auto * Page = new QWidget(this);
    auto * Layout = new QVBoxLayout(Page);
    Layout->setContentsMargins(0, 0, 0, 0);
    FKeyIntro = Text(QString(), Page);
    Layout->addWidget(FKeyIntro);
    auto * Form = new QFormLayout();
    FPassword = new QLineEdit(Page);
    FPassword->setEchoMode(QLineEdit::Password);
    FPasswordConfirm = new QLineEdit(Page);
    FPasswordConfirm->setEchoMode(QLineEdit::Password);
    FConfirmLabel = new QLabel(QStringLiteral("Repeat:"), Page);
    Form->addRow(QStringLiteral("Sync password:"), FPassword);
    Form->addRow(FConfirmLabel, FPasswordConfirm);
    Layout->addLayout(Form);
    FRecoveryCheck = new QCheckBox(QStringLiteral("Create a recovery key (recommended)"), Page);
    FRecoveryCheck->setChecked(true);
    Layout->addWidget(FRecoveryCheck);
    FKeyError = Text(QString(), Page);
    FKeyError->setStyleSheet(QStringLiteral("color: #cf222e"));
    Layout->addWidget(FKeyError);
    auto * Row = new QHBoxLayout();
    FKeyButton = new QPushButton(Page);
    FKeyButton->setDefault(true);
    FUseRecoveryKey = new QToolButton(Page);
    FUseRecoveryKey->setAutoRaise(true);
    auto * SignOut = new QToolButton(Page);
    SignOut->setAutoRaise(true);
    SignOut->setText(QStringLiteral("Sign out"));
    Row->addWidget(FKeyButton);
    Row->addWidget(FUseRecoveryKey);
    Row->addStretch();
    Row->addWidget(SignOut);
    Layout->addLayout(Row);
    Layout->addStretch();
    connect(FKeyButton, &QPushButton::clicked, this, [this]() { SubmitKey(); });
    connect(FPassword, &QLineEdit::returnPressed, this, [this]() { SubmitKey(); });
    connect(FPasswordConfirm, &QLineEdit::returnPressed, this, [this]() { SubmitKey(); });
    connect(FUseRecoveryKey, &QToolButton::clicked, this, [this]()
    {
      FRecoveryMode = !FRecoveryMode;
      FPassword->clear();
      UpdateKeyPage();
    });
    connect(SignOut, &QToolButton::clicked, this, []() { CloudSync::Instance().SignOut(); });
    return Page;
  }

  QWidget * CreateReadyPage()
  {
    auto * Page = new QWidget(this);
    auto * Layout = new QVBoxLayout(Page);
    Layout->setContentsMargins(0, 0, 0, 0);
    auto * AccountBox = new QGroupBox(QStringLiteral("Account"), Page);
    auto * AccountLayout = new QVBoxLayout(AccountBox);
    FAccount = Text(QString(), AccountBox);
    FLicense = Text(QString(), AccountBox);
    AccountLayout->addWidget(FAccount);
    AccountLayout->addWidget(FLicense);
    Layout->addWidget(AccountBox);
    auto * StatusBox = new QGroupBox(QStringLiteral("Status"), Page);
    auto * StatusLayout = new QVBoxLayout(StatusBox);
    FStatus = Text(QString(), StatusBox);
    StatusLayout->addWidget(FStatus);
    Layout->addWidget(StatusBox);
    auto * OptionsBox = new QGroupBox(QStringLiteral("On this computer"), Page);
    auto * OptionsLayout = new QVBoxLayout(OptionsBox);
    FSyncSessions = new QCheckBox(QStringLiteral("Sync open sessions (tabs)"), OptionsBox);
    FRestoreSessions = new QCheckBox(QStringLiteral("Reopen the last open sessions at startup"), OptionsBox);
    auto * Protection = Text(QStringLiteral(
      "Your sync password is your NixSCP password: it also encrypts the configuration on this computer "
      "(sites, passwords and preferences), and NixSCP asks for it when it starts."), OptionsBox);
    OptionsLayout->addWidget(FSyncSessions);
    OptionsLayout->addWidget(FRestoreSessions);
    OptionsLayout->addWidget(Protection);
    Layout->addWidget(OptionsBox);
    auto * Row = new QHBoxLayout();
    auto * SyncNow = new QPushButton(QStringLiteral("Sync now"), Page);
    auto * ChangePassword = new QPushButton(QStringLiteral("Change NixSCP password..."), Page);
    auto * Manage = new QPushButton(QStringLiteral("Manage account"), Page);
    auto * SignOut = new QPushButton(QStringLiteral("Sign out"), Page);
    Row->addWidget(SyncNow);
    Row->addWidget(ChangePassword);
    Row->addWidget(Manage);
    Row->addStretch();
    Row->addWidget(SignOut);
    Layout->addLayout(Row);
    Layout->addStretch();
    CloudSync & Sync = CloudSync::Instance();
    connect(FSyncSessions, &QCheckBox::toggled, this, [&Sync](bool On) { Sync.SetSyncSessions(On); });
    connect(FRestoreSessions, &QCheckBox::toggled, this, [&Sync](bool On) { Sync.SetRestoreSessions(On); });
    connect(SyncNow, &QPushButton::clicked, this, [&Sync]() { Sync.SyncNow(); });
    connect(ChangePassword, &QPushButton::clicked, this, [this]() { DoChangePassword(); });
    connect(Manage, &QPushButton::clicked, this, [&Sync]()
    {
      QDesktopServices::openUrl(QUrl(Sync.Client().BaseUrl() + QStringLiteral("/account")));
    });
    connect(SignOut, &QPushButton::clicked, this, [this, &Sync]()
    {
      if (QMessageBox::question(this, QStringLiteral("Sign out"),
            QStringLiteral("Sign out of the NixSCP cloud on this computer?\n\nYour settings stay on this computer and in the cloud.")) ==
          QMessageBox::Yes)
      {
        Sync.SignOut();
      }
    });
    return Page;
  }

  void Update()
  {
    CloudSync & Sync = CloudSync::Instance();
    int Page = 2;
    if ((Sync.State() == CloudSync::csDisabled) || !Sync.SignedIn())
    {
      Page = 0;
    }
    else if (!Sync.HasKey())
    {
      Page = 1;
    }
    if (Page != FShownPage)
    {
      FShownPage = Page;
      FPages->setCurrentIndex(Page);
      if (Page == 1)
      {
        FProbed = false;
        FRecoveryMode = false;
        FPassword->clear();
        FPasswordConfirm->clear();
        FKeyError->clear();
        Sync.ProbeCloud([Guard = QPointer<CloudSyncDialog>(this)](bool Ok, bool HasData, const QString & Error)
        {
          if (Guard)
          {
            Guard->FProbed = Ok;
            Guard->FCloudHasData = HasData;
            Guard->FKeyError->setText(Ok ? QString() : Error);
            Guard->UpdateKeyPage();
          }
        });
      }
      if (Page == 2)
      {
        FLicense->setText(QStringLiteral("Checking license..."));
        Sync.FetchAccount([Guard = QPointer<CloudSyncDialog>(this)](bool Ok, const QJsonObject & Me)
        {
          if (Guard)
          {
            Guard->ShowLicense(Ok, Me);
          }
        });
      }
    }
    switch (Page)
    {
      case 0:
        FSignInButton->setEnabled((Sync.State() != CloudSync::csDisabled) && FSignIn.isNull());
        FRemoveEncryption->setVisible(ConfigVault::Enabled());
        FSignedOutStatus->setText((Sync.State() == CloudSync::csDisabled) ? Sync.StatusText()
          : (FSignIn ? QStringLiteral("<i>Waiting for you to confirm in the browser...</i>")
                     : (Sync.LastError().isEmpty() ? QString() : Sync.LastError().toHtmlEscaped())));
        break;
      case 1:
        UpdateKeyPage();
        break;
      default:
        FAccount->setText(QStringLiteral("Signed in as <b>%1</b>").arg(Sync.Email().toHtmlEscaped()));
        FStatus->setText(Sync.StatusText().toHtmlEscaped());
        QSignalBlocker B1(FSyncSessions), B2(FRestoreSessions);
        FSyncSessions->setChecked(Sync.SyncSessions());
        FRestoreSessions->setChecked(Sync.RestoreSessions());
        break;
    }
  }

  void ShowLicense(bool Ok, const QJsonObject & Me)
  {
    QJsonObject License = Me.value(QStringLiteral("license")).toObject();
    QString Link = QStringLiteral("<a href=\"%1/account\">Get NixSCP Pro</a>").arg(CloudSync::Instance().Client().BaseUrl());
    if (!Ok)
    {
      FLicense->setText(QStringLiteral("License status is not available right now."));
    }
    else if (License.value(QStringLiteral("plan")).toString() == QStringLiteral("beta"))
    {
      FLicense->setText(QStringLiteral("NixSCP Pro beta: cloud sync is free while the beta lasts. "
        "NixSCP will tell you when the beta ends; your synced data is kept."));
    }
    else if (License.value(QStringLiteral("active")).toBool())
    {
      bool Lifetime = (License.value(QStringLiteral("plan")).toString() == QStringLiteral("lifetime"));
      FLicense->setText(Lifetime ? QStringLiteral("NixSCP Pro lifetime license.") : QStringLiteral("NixSCP Pro monthly subscription."));
    }
    else if (License.value(QStringLiteral("beta_ended")).toBool())
    {
      FLicense->setText(QStringLiteral("The NixSCP Pro beta has ended: syncing can download but not upload. ") + Link);
    }
    else
    {
      FLicense->setText(QStringLiteral("No active NixSCP Pro license: syncing can download but not upload. ") + Link);
    }
  }

  void UpdateKeyPage()
  {
    bool Create = FProbed && !FCloudHasData;
    if (!FProbed)
    {
      FKeyIntro->setText(QStringLiteral("<h3>Sync password</h3><p>Checking the cloud...</p>"));
    }
    else if (Create)
    {
      FKeyIntro->setText(QStringLiteral(
        "<h3>Choose a sync password</h3>"
        "<p>Your data is encrypted with it on this computer before upload. Use the same password on your other computers.</p>"
        "<p><b>It never leaves your computers: nobody, not even NixSCP support, can reset it.</b> "
        "Without it (or the recovery key) your synced data cannot be decrypted.</p>"));
    }
    else
    {
      FKeyIntro->setText(FRecoveryMode
        ? QStringLiteral("<h3>Enter your recovery key</h3><p>The key you saved when you set up syncing on your first computer.</p>")
        : QStringLiteral("<h3>Enter your sync password</h3><p>The password you chose on your first computer.</p>"));
    }
    FPassword->setEnabled(FProbed);
    FPassword->setEchoMode(FRecoveryMode ? QLineEdit::Normal : QLineEdit::Password);
    FPassword->setPlaceholderText(FRecoveryMode ? QStringLiteral("XXXX-XXXX-...") : QString());
    FConfirmLabel->setVisible(Create);
    FPasswordConfirm->setVisible(Create);
    FRecoveryCheck->setVisible(Create);
    FUseRecoveryKey->setVisible(FProbed && !Create);
    FUseRecoveryKey->setText(FRecoveryMode ? QStringLiteral("Use the sync password") : QStringLiteral("I forgot it: use the recovery key"));
    FKeyButton->setText(Create ? QStringLiteral("Start syncing") : QStringLiteral("Unlock"));
    FKeyButton->setEnabled(FProbed);
  }

  void SetBusy(bool Busy)
  {
    FKeyButton->setEnabled(!Busy);
    FPassword->setEnabled(!Busy);
    FPasswordConfirm->setEnabled(!Busy);
    if (Busy)
    {
      FKeyError->setText(QStringLiteral("<i>Working...</i>"));
    }
  }

  void SubmitKey()
  {
    if (!FProbed || !FKeyButton->isEnabled())
    {
      return;
    }
    CloudSync & Sync = CloudSync::Instance();
    QPointer<CloudSyncDialog> Guard(this);
    if (!FCloudHasData)
    {
      if (FPassword->text().size() < MinSyncPasswordLength)
      {
        FKeyError->setText(QStringLiteral("Use at least %1 characters.").arg(MinSyncPasswordLength));
        return;
      }
      if (FPassword->text() != FPasswordConfirm->text())
      {
        FKeyError->setText(QStringLiteral("The passwords do not match."));
        return;
      }
      SetBusy(true);
      Sync.CreateKey(FPassword->text(), FRecoveryCheck->isChecked(), [Guard](bool Ok, const QString & Result)
      {
        if (!Guard)
        {
          return;
        }
        Guard->SetBusy(false);
        if (!Ok)
        {
          Guard->FKeyError->setText(Result.toHtmlEscaped());
          return;
        }
        if (!Result.isEmpty())
        {
          ShowRecoveryKey(Guard, Result);
        }
      });
      return;
    }
    SetBusy(true);
    Sync.UnlockKey(FPassword->text(), FRecoveryMode, [Guard](bool Ok, const QString & Error)
    {
      if (!Guard)
      {
        return;
      }
      Guard->SetBusy(false);
      if (!Ok)
      {
        Guard->FKeyError->setText(Error.toHtmlEscaped());
        return;
      }
      if (Guard->FRecoveryMode)
      {
        // The old password is forgotten: choose a new one right away
        Guard->DoChangePassword();
      }
    });
  }

  static void ShowRecoveryKey(QWidget * Parent, const QString & Key)
  {
    QDialog Dialog(Parent);
    Dialog.setWindowTitle(QStringLiteral("Your recovery key"));
    auto * Layout = new QVBoxLayout(&Dialog);
    Layout->addWidget(Text(QStringLiteral(
      "<p>If you forget your sync password, this key is the only way to decrypt your synced data. "
      "Print it or store it in your password manager now; it is not shown again.</p>"), &Dialog));
    auto * Edit = new QPlainTextEdit(Key, &Dialog);
    Edit->setReadOnly(true);
    QFont Mono(QStringLiteral("monospace"));
    Mono.setStyleHint(QFont::Monospace);
    Edit->setFont(Mono);
    Edit->setFixedHeight(Edit->fontMetrics().height() * 4);
    Layout->addWidget(Edit);
    auto * Buttons = new QDialogButtonBox(&Dialog);
    Buttons->addButton(QStringLiteral("I have saved it"), QDialogButtonBox::AcceptRole);
    connect(Buttons, &QDialogButtonBox::accepted, &Dialog, &QDialog::accept);
    Layout->addWidget(Buttons);
    Dialog.exec();
  }

  void DoChangePassword()
  {
    QDialog Dialog(this);
    Dialog.setWindowTitle(QStringLiteral("Change sync password"));
    auto * Layout = new QVBoxLayout(&Dialog);
    Layout->addWidget(Text(QStringLiteral(
      "<p>The new password applies on all your computers; the ones already syncing keep working. "
      "Your recovery key stays valid.</p>"), &Dialog));
    auto * Form = new QFormLayout();
    auto * New = new QLineEdit(&Dialog);
    New->setEchoMode(QLineEdit::Password);
    auto * Repeat = new QLineEdit(&Dialog);
    Repeat->setEchoMode(QLineEdit::Password);
    Form->addRow(QStringLiteral("New sync password:"), New);
    Form->addRow(QStringLiteral("Repeat:"), Repeat);
    Layout->addLayout(Form);
    auto * Error = Text(QString(), &Dialog);
    Error->setStyleSheet(QStringLiteral("color: #cf222e"));
    Layout->addWidget(Error);
    auto * Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &Dialog);
    Layout->addWidget(Buttons);
    connect(Buttons, &QDialogButtonBox::rejected, &Dialog, &QDialog::reject);
    connect(Buttons, &QDialogButtonBox::accepted, &Dialog, [&]()
    {
      if (New->text().size() < MinSyncPasswordLength)
      {
        Error->setText(QStringLiteral("Use at least %1 characters.").arg(MinSyncPasswordLength));
      }
      else if (New->text() != Repeat->text())
      {
        Error->setText(QStringLiteral("The passwords do not match."));
      }
      else
      {
        CloudSync::Instance().ChangePassword(New->text(), [&](bool Ok, const QString & Message)
        {
          if (Ok)
          {
            Dialog.accept();
          }
          else
          {
            Error->setText(Message.toHtmlEscaped());
          }
        });
      }
    });
    Dialog.exec();
  }

  void SignIn()
  {
    QPointer<CloudSyncDialog> Guard(this);
    auto * Session = new LoopbackSignIn(this, [Guard](bool Ok, const QString & Error)
    {
      if (!Guard)
      {
        return;
      }
      if (Guard->FSignIn)
      {
        Guard->FSignIn->deleteLater();
      }
      Guard->FSignIn = nullptr;
      Guard->Update();
      if (!Ok)
      {
        Guard->FSignedOutStatus->setText(Error.toHtmlEscaped());
      }
      Guard->raise();
      Guard->activateWindow();
    });
    FSignIn = Session;
    if (!Session->Start())
    {
      FSignedOutStatus->setText(QStringLiteral("Cannot open the browser. Open this address manually:<br><a href=\"%1\">%2</a>")
        .arg(Session->Url().toHtmlEscaped(), Session->Url().left(60).toHtmlEscaped() + QStringLiteral("...")));
      FSignedOutStatus->setTextInteractionFlags(Qt::TextBrowserInteraction);
    }
    Update();
  }
};

void ShowDialog(QWidget * Parent)
{
  CloudSyncDialog Dialog(Parent);
  Dialog.exec();
}

//---------------------------------------------------------------------------
// Top menu

static void UpdateMenuTitle(QMenu * Menu)
{
  Menu->setTitle(CloudSync::Instance().SignedIn() ? QStringLiteral("Pro") : QStringLiteral("Cloud Sync"));
}

static void FillMenu(QMenu * Menu, QWidget * Window)
{
  Menu->clear();
  CloudSync & Sync = CloudSync::Instance();
  QString BaseUrl = Sync.Client().BaseUrl();
  auto Open = [BaseUrl](const QString & Path) { QDesktopServices::openUrl(QUrl(BaseUrl + Path)); };
  if (Sync.State() == CloudSync::csDisabled)
  {
    Menu->addAction(Sync.StatusText())->setEnabled(false);
    return;
  }
  if (!Sync.SignedIn())
  {
    Menu->addAction(QStringLiteral("Sign In..."), Window, [Window]() { ShowDialog(Window); });
    Menu->addAction(QStringLiteral("Create Account..."), Window, [Open]() { Open(QStringLiteral("/login?register&next=/account")); });
    Menu->addSeparator();
    Menu->addAction(QStringLiteral("What Is NixSCP Pro?"), Window, [Open]() { Open(QStringLiteral("/")); });
    return;
  }
  QAction * Who = Menu->addAction(QStringLiteral("NixSCP Pro: %1").arg(Sync.Email()));
  Who->setEnabled(false);
  if (Sync.State() == CloudSync::csNoLicense)
  {
    Menu->addAction(QStringLiteral("Renew NixSCP Pro..."), Window, [Open]() { Open(QStringLiteral("/account")); });
  }
  QAction * Status = Menu->addAction(Sync.StatusText());
  Status->setEnabled(false);
  Menu->addSeparator();
  if (!Sync.HasKey())
  {
    Menu->addAction(QStringLiteral("Enter Sync Password..."), Window, [Window]() { ShowDialog(Window); });
  }
  else
  {
    QAction * Now = Menu->addAction(QStringLiteral("Sync Now"), Window, [&Sync]() { Sync.SyncNow(); });
    Now->setEnabled(Sync.State() != CloudSync::csSyncing);
  }
  Menu->addAction(QStringLiteral("Cloud Sync Settings..."), Window, [Window]() { ShowDialog(Window); });
  Menu->addSeparator();
  Menu->addAction(QStringLiteral("Manage Account..."), Window, [Open]() { Open(QStringLiteral("/account")); });
  Menu->addSeparator();
  Menu->addAction(QStringLiteral("Sign Out"), Window, [Window, &Sync]()
  {
    if (QMessageBox::question(Window, QStringLiteral("Sign out"),
          QStringLiteral("Sign out of the NixSCP cloud on this computer?\n\nYour settings stay on this computer and in the cloud.")) ==
        QMessageBox::Yes)
    {
      Sync.SignOut();
    }
  });
}

void SetupMenu(QMenu * Menu, QWidget * Window)
{
  UpdateMenuTitle(Menu);
  QObject::connect(Menu, &QMenu::aboutToShow, Menu, [Menu, Window]() { FillMenu(Menu, Window); });
  QObject::connect(&CloudSync::Instance(), &CloudSync::StateChanged, Menu, [Menu]() { UpdateMenuTitle(Menu); });
  // Keyboard navigation needs entries before the first opening
  FillMenu(Menu, Window);
}

//---------------------------------------------------------------------------
// Configuration backups

bool ExportConfiguration(QWidget * Parent, const QString & FileName, QString * Error)
{
  IniFile Out = Configuration::Instance().ExportData();
  CloudSync & Sync = CloudSync::Instance();
  if (ConfigVault::Enabled())
  {
    // NixSCP Pro: an encrypted backup, or a plain file WinSCP can read
    QMessageBox Box(QMessageBox::Question, QStringLiteral("Export Configuration"),
      QStringLiteral("How do you want to export your configuration?"), QMessageBox::NoButton, Parent);
    Box.setInformativeText(QStringLiteral(
      "Encrypted backup: opens in NixSCP on any computer with your NixSCP password or recovery key. "
      "Safe to keep anywhere, but WinSCP cannot read it.\n\n"
      "Plain file: a WinSCP-compatible configuration that WinSCP and NixSCP can import. Your site passwords "
      "are only obfuscated in it, so anyone who gets the file can read them."));
    QPushButton * Encrypted = Box.addButton(QStringLiteral("Encrypted backup"), QMessageBox::AcceptRole);
    QPushButton * Plain = Box.addButton(QStringLiteral("Plain file for WinSCP"), QMessageBox::AcceptRole);
    QPushButton * Cancel = Box.addButton(QMessageBox::Cancel);
    Box.setDefaultButton(Encrypted);
    Box.setEscapeButton(Cancel);
    Box.exec();
    if (Box.clickedButton() == Encrypted)
    {
      QSaveFile File(FileName);
      if (!File.open(QIODevice::WriteOnly) || (File.write(ConfigVault::SealExport(Out.SaveToData())) < 0) || !File.commit())
      {
        if (Error != nullptr)
        {
          *Error = QStringLiteral("Error writing to file \"%1\".").arg(FileName);
        }
        return false;
      }
      QFile::setPermissions(FileName, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
      return true;
    }
    if (Box.clickedButton() != Plain)
    {
      // cancelled: nothing written, no error
      if (Error != nullptr)
      {
        Error->clear();
      }
      return false;
    }
  }
  if (Sync.HasKey())
  {
    QMessageBox Box(QMessageBox::Question, QStringLiteral("Export Configuration"),
      QStringLiteral("Include your cloud sync key in the backup?"), QMessageBox::NoButton, Parent);
    Box.setInformativeText(QStringLiteral(
      "With the key, importing this backup on another computer starts syncing without asking for the sync password.\n\n"
      "Anyone who gets the file can then decrypt your synced data, so keep it as safe as your sync password. "
      "WinSCP can still import the backup; it ignores the key."));
    QPushButton * Without = Box.addButton(QStringLiteral("Export without key"), QMessageBox::RejectRole);
    QPushButton * With = Box.addButton(QStringLiteral("Include key"), QMessageBox::AcceptRole);
    Box.setDefaultButton(Without);
    Box.setEscapeButton(Without);
    Box.exec();
    if (Box.clickedButton() == With)
    {
      Sync.ExportKey(Out);
    }
  }
  QFile::remove(FileName);
  if (!Out.Save(FileName))
  {
    if (Error != nullptr)
    {
      *Error = QStringLiteral("Error writing to file \"%1\".").arg(FileName);
    }
    return false;
  }
  // Stored passwords (and maybe the sync key): readable by the owner only
  QFile::setPermissions(FileName, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  return true;
}

bool ImportConfiguration(QWidget * Parent, const QString & FileName, QString * Error)
{
  IniFile In;
  QFile Raw(FileName);
  QByteArray Data = Raw.open(QIODevice::ReadOnly) ? Raw.readAll() : QByteArray();
  if (ConfigVault::IsSealed(Data))
  {
    // an encrypted NixSCP backup: this computer's key, or its own password
    QByteArray Plaintext;
    if (!UnlockDialog::OpenEncryptedBackup(Parent, FileName, Data, Plaintext))
    {
      if (Error != nullptr)
      {
        Error->clear();
      }
      return false;
    }
    In.LoadFromData(Plaintext);
  }
  else if (!QFileInfo(FileName).isFile() || !In.Load(FileName))
  {
    if (Error != nullptr)
    {
      *Error = QStringLiteral("Cannot open file \"%1\".").arg(FileName);
    }
    return false;
  }
  // ImportData leaves the key section out of the configuration itself
  if (!Configuration::Instance().ImportData(In, Error))
  {
    return false;
  }
  CloudSync & Sync = CloudSync::Instance();
  QString Email;
  switch (Sync.ImportKey(In, &Email))
  {
    case CloudSync::kiNone:
      break;
    case CloudSync::kiImported:
      QMessageBox::information(Parent, QStringLiteral("Cloud Sync"), Sync.SignedIn()
        ? QStringLiteral("The backup contains your cloud sync key; syncing continues without asking for the sync password.")
        : QStringLiteral("The backup contains the cloud sync key of %1. Sign in with this account in Options > Cloud Sync "
            "to start syncing without entering the sync password.").arg(Email));
      break;
    case CloudSync::kiOtherAccount:
      QMessageBox::warning(Parent, QStringLiteral("Cloud Sync"),
        QStringLiteral("The backup contains the cloud sync key of another account (%1). It was not used.").arg(Email));
      break;
    case CloudSync::kiInvalid:
      QMessageBox::warning(Parent, QStringLiteral("Cloud Sync"),
        QStringLiteral("The cloud sync key in the backup is damaged or not supported. It was not used."));
      break;
  }
  return true;
}

//---------------------------------------------------------------------------
// Main window integration

class Controller : public QObject
{
public:
  explicit Controller(CommanderWindow * Window) : QObject(Window), FWindow(Window)
  {
    FIndicator = new QToolButton(Window);
    FIndicator->setAutoRaise(true);
    FIndicator->setToolButtonStyle(Qt::ToolButtonTextOnly);
    Window->statusBar()->addPermanentWidget(FIndicator);
    connect(FIndicator, &QToolButton::clicked, Window, [Window]() { ShowDialog(Window); });
    // NixSCP Teams: shortly after the start, then every minute
    FTeamsTimer = new QTimer(this);
    FTeamsTimer->setInterval(TeamsIntervalMs);
    auto SyncTeams = []()
    {
      if (TeamSync::Instance().Available())
      {
        TeamSync::Instance().SyncAll();
      }
    };
    connect(FTeamsTimer, &QTimer::timeout, this, SyncTeams);
    FTeamsTimer->start();
    QTimer::singleShot(TeamsFirstSyncMs, this, SyncTeams);
    connect(&TeamSync::Instance(), &TeamSync::Notice, this, [this](const QString & Message)
    {
      if (FClosing)
      {
        return;
      }
      auto * Box = new QMessageBox(QMessageBox::Information, QStringLiteral("NixSCP Teams"), Message, QMessageBox::Ok, FWindow);
      Box->setAttribute(Qt::WA_DeleteOnClose);
      Box->setWindowModality(Qt::NonModal);
      Box->show();
    });
    connect(&TeamSync::Instance(), &TeamSync::SitesReceived, this,
      [this](const QString & TeamName, int Added, int Removed, int Changed)
    {
      QStringList Parts;
      if (Added > 0) Parts.append(QStringLiteral("%1 added").arg(Added));
      if (Changed > 0) Parts.append(QStringLiteral("%1 changed").arg(Changed));
      if (Removed > 0) Parts.append(QStringLiteral("%1 removed").arg(Removed));
      FWindow->statusBar()->showMessage(QStringLiteral("Team \"%1\": sites %2.").arg(TeamName, Parts.join(QStringLiteral(", "))), 10000);
    });
    // the license, also without uploads (e.g. when the beta ends): soon after
    // the start, then daily
    FLicenseTimer = new QTimer(this);
    FLicenseTimer->setInterval(LicenseCheckIntervalMs);
    connect(FLicenseTimer, &QTimer::timeout, this, []() { CloudSync::Instance().CheckLicense(); });
    FLicenseTimer->start();
    QTimer::singleShot(LicenseFirstCheckMs, this, []() { CloudSync::Instance().CheckLicense(); });
    FSessionTimer = new QTimer(this);
    FSessionTimer->setSingleShot(true);
    FSessionTimer->setInterval(SessionIntervalMs);
    connect(FSessionTimer, &QTimer::timeout, this, [this]()
    {
      QByteArray Payload = SessionPayload(FWindow->WorkspaceTabs());
      // Browsing back to where it was is not worth an upload
      if (Payload != FLastSessionPayload)
      {
        FLastSessionPayload = Payload;
        CloudSync::Instance().PushSession(Payload);
      }
    });
    Window->installEventFilter(this);
    CloudSync & Sync = CloudSync::Instance();
    connect(&Sync, &CloudSync::StateChanged, this, [this]() { UpdateIndicator(); });
    connect(&Sync, &CloudSync::SettingsPulled, this, [this](const QString & Device)
    {
      FWindow->statusBar()->showMessage(Device.isEmpty() ? QStringLiteral("Settings synced from the cloud.")
                                                          : QStringLiteral("Settings synced from %1.").arg(Device), 8000);
    });
    connect(&Sync, &CloudSync::SignedOutRemotely, this, [this](const QString & Message)
    {
      QTimer::singleShot(0, this, [this, Message]() { NotifySignedOut(Message); });
    });
    connect(&Sync, &CloudSync::LicenseInactive, this, [this]()
    {
      QTimer::singleShot(0, this, [this]() { NotifyLicense(); });
    });
    connect(&Sync, &CloudSync::ForeignSnapshotIgnored, this, [this](const QString & Device)
    {
      QTimer::singleShot(0, this, [this, Device]() { NotifyForeignSnapshot(Device); });
    });
    connect(&Sync, &CloudSync::ConflictDetected, this, [this](const QString & Device, const QDateTime & Time)
    {
      // Queued: the prompt must not run inside the network callback
      QTimer::singleShot(0, this, [this, Device, Time]() { AskConflict(Device, Time); });
    });
    UpdateIndicator();
  }

  void Closing()
  {
    FClosing = true;
    FSessionTimer->stop();
  }

  void SessionsChanged()
  {
    CloudSync & Sync = CloudSync::Instance();
    // Not restarted by further changes: continuous browsing still uploads once a minute
    if (Sync.SyncSessions() && Sync.HasKey() && Sync.SignedIn() && !FSessionTimer->isActive())
    {
      FSessionTimer->start();
    }
  }

private:
  CommanderWindow * FWindow;
  QToolButton * FIndicator;
  QTimer * FSessionTimer;
  QTimer * FLicenseTimer;
  QTimer * FTeamsTimer;
  QByteArray FLastSessionPayload;
  bool FClosing = false;

  bool eventFilter(QObject * Object, QEvent * Event) override
  {
    // Back at this computer: pick up what changed on the others
    if ((Object == FWindow) && (Event->type() == QEvent::WindowActivate) && !FClosing)
    {
      CloudSync::Instance().SyncIfStale(FocusCheckSeconds);
    }
    return QObject::eventFilter(Object, Event);
  }

  void UpdateIndicator()
  {
    CloudSync & Sync = CloudSync::Instance();
    QString Text;
    switch (Sync.State())
    {
      case CloudSync::csDisabled:
      case CloudSync::csSignedOut:
        // Not a Pro user (or signed out on purpose): no clutter, unless the server signed us out
        Text = Sync.LastError().isEmpty() ? QString() : QStringLiteral("Cloud: signed out");
        break;
      case CloudSync::csNeedKey: Text = QStringLiteral("Cloud: enter sync password"); break;
      case CloudSync::csIdle: Text = QStringLiteral("Cloud: synced"); break;
      case CloudSync::csSyncing: Text = QStringLiteral("Cloud: syncing..."); break;
      case CloudSync::csConflict:
      case CloudSync::csPaused: Text = QStringLiteral("Cloud: conflict"); break;
      case CloudSync::csNoLicense: Text = QStringLiteral("Cloud: no license"); break;
      case CloudSync::csError: Text = QStringLiteral("Cloud: offline"); break;
    }
    FIndicator->setText(Text);
    FIndicator->setToolTip(Sync.StatusText());
    FIndicator->setVisible(!Text.isEmpty());
  }

  void OpenAccount()
  {
    QDesktopServices::openUrl(QUrl(CloudSync::Instance().Client().BaseUrl() + QStringLiteral("/account")));
  }

  void NotifySignedOut(const QString & Message)
  {
    if (FClosing)
    {
      return;
    }
    QMessageBox Box(QMessageBox::Information, QStringLiteral("Cloud Sync"), Message, QMessageBox::NoButton, FWindow);
    QPushButton * SignIn = Box.addButton(QStringLiteral("Sign In..."), QMessageBox::AcceptRole);
    Box.addButton(QStringLiteral("Later"), QMessageBox::RejectRole);
    Box.setDefaultButton(SignIn);
    Box.exec();
    if (Box.clickedButton() == SignIn)
    {
      ShowDialog(FWindow);
    }
  }

  void NotifyLicense()
  {
    // the account tells whether the beta ended or a license lapsed
    CloudSync::Instance().FetchAccount([Guard = QPointer<Controller>(this)](bool Ok, const QJsonObject & Me)
    {
      if (Guard != nullptr)
      {
        Guard->ShowLicenseNotice(Ok && Me.value(QStringLiteral("license")).toObject().value(QStringLiteral("beta_ended")).toBool());
      }
    });
  }

  void ShowLicenseNotice(bool BetaEnded)
  {
    if (FClosing)
    {
      return;
    }
    QMessageBox Box(QMessageBox::Information, QStringLiteral("NixSCP Pro"),
      BetaEnded ?
        QStringLiteral("The NixSCP Pro beta has ended. Thank you for testing cloud sync!") :
        QStringLiteral("Your NixSCP Pro license is not active, so changes on this computer are no longer uploaded."),
      QMessageBox::NoButton, FWindow);
    Box.setInformativeText(BetaEnded ?
      QStringLiteral("Cloud sync is now part of NixSCP Pro. Changes on this computer are no longer uploaded, but nothing is lost: "
        "your synced settings stay in the cloud and still download, and uploading resumes as soon as you get NixSCP Pro. "
        "NixSCP itself stays free.") :
      QStringLiteral("Settings from your other computers still download, and nothing is lost: "
        "your changes upload as soon as NixSCP Pro is renewed."));
    QPushButton * Renew = Box.addButton(BetaEnded ? QStringLiteral("Get NixSCP Pro...") : QStringLiteral("Renew..."), QMessageBox::AcceptRole);
    Box.addButton(QStringLiteral("Later"), QMessageBox::RejectRole);
    Box.setDefaultButton(Renew);
    Box.exec();
    if (Box.clickedButton() == Renew)
    {
      OpenAccount();
    }
  }

  void NotifyForeignSnapshot(const QString & Device)
  {
    if (FClosing)
    {
      return;
    }
    QMessageBox Box(QMessageBox::Warning, QStringLiteral("Cloud Sync"),
      QStringLiteral("NixSCP ignored settings uploaded by \"%1\" that were not encrypted with your sync password.")
        .arg(Device.isEmpty() ? QStringLiteral("an unknown computer") : Device),
      QMessageBox::NoButton, FWindow);
    Box.setInformativeText(QStringLiteral("Your settings are safe: that computer cannot read them, and valid settings were uploaded again. "
      "If you do not recognize it, remove it from the Computers list of your account and change your account password "
      "with \"Also sign out all computers\"."));
    QPushButton * Manage = Box.addButton(QStringLiteral("Manage Account..."), QMessageBox::AcceptRole);
    Box.addButton(QStringLiteral("Close"), QMessageBox::RejectRole);
    Box.exec();
    if (Box.clickedButton() == Manage)
    {
      OpenAccount();
    }
  }

  void AskConflict(const QString & Device, const QDateTime & Time)
  {
    CloudSync & Sync = CloudSync::Instance();
    // While closing the conflict waits for the next start
    if (FClosing || (Sync.State() != CloudSync::csConflict))
    {
      return;
    }
    QMessageBox Box(QMessageBox::Question, QStringLiteral("Cloud Sync"),
      QStringLiteral("Your settings were changed on this computer and also on %1%2.\n\n"
        "Which settings do you want to keep? The other version is not lost: "
        "local settings are backed up before they are replaced, and the cloud keeps older versions.")
        .arg(Device.isEmpty() ? QStringLiteral("another computer") : Device)
        .arg(Time.isValid() ? QStringLiteral(" (%1)").arg(QLocale().toString(Time.toLocalTime(), QLocale::ShortFormat)) : QString()),
      QMessageBox::NoButton, FWindow);
    QPushButton * Cloud = Box.addButton(QStringLiteral("Use cloud settings"), QMessageBox::AcceptRole);
    QPushButton * Local = Box.addButton(QStringLiteral("Keep this computer's settings"), QMessageBox::DestructiveRole);
    QPushButton * Later = Box.addButton(QStringLiteral("Decide later"), QMessageBox::RejectRole);
    Box.setDefaultButton(Cloud);
    Box.exec();
    if (Box.clickedButton() == Cloud)
    {
      Sync.ResolveConflict(CloudSync::crUseCloud);
    }
    else if (Box.clickedButton() == Local)
    {
      Sync.ResolveConflict(CloudSync::crUseLocal);
    }
    else
    {
      Q_UNUSED(Later);
      Sync.ResolveConflict(CloudSync::crLater);
    }
  }
};

void Attach(CommanderWindow * Window)
{
  Window->setProperty("CloudSyncController", QVariant::fromValue(static_cast<QObject *>(new Controller(Window))));
}

static Controller * ControllerOf(CommanderWindow * Window)
{
  return static_cast<Controller *>(Window->property("CloudSyncController").value<QObject *>());
}

void SessionsChanged(CommanderWindow * Window)
{
  if (Controller * C = ControllerOf(Window))
  {
    C->SessionsChanged();
  }
}

void FlushOnExit(CommanderWindow * Window, const QList<SessionData> & Tabs)
{
  CloudSync & Sync = CloudSync::Instance();
  if ((Sync.State() == CloudSync::csDisabled) || !Sync.HasKey() || !Sync.SignedIn())
  {
    return;
  }
  if (Controller * C = ControllerOf(Window))
  {
    C->Closing();
  }
  Sync.FlushSettings(FlushTimeoutMs);
  if (Sync.SyncSessions())
  {
    Sync.FlushSession(SessionPayload(Tabs), FlushTimeoutMs);
  }
}

}
