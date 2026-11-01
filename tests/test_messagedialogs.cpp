// Message dialog and login prompt tests (WP19): src/gui/GuiUserInterface
// (TMessageForm port: answer buttons in WinSCP order, aliases and split
// buttons with Shift/Ctrl/Alt variants, "Never ask me again" returning
// qaNeverAskAgain, countdown on the default button, Help button, icons and
// captions by query type), src/gui/AuthenticateDialog (one Authenticate
// window: connection log, prompts with "Remember password for this session"
// and "Change stored password to this one", banner with "Never show this
// banner again" stored in Configuration\Banners and monospaced font), and
// the host key prompt (Accept / Connect Once / Paste key / Copy key).
//
// GUI test: built with NIXSCP_GUI_TESTS and the GUI objects, runs offscreen.
// No server needed. NIXSCP_TEST_SCREENSHOT=<dir> saves PNGs of the dialogs.
#ifndef NIXSCP_GUI_TESTS

#include <cstdio>

int main()
{
  std::printf("SKIP: test_messagedialogs needs NIXSCP_GUI_TESTS (Qt Widgets + the GUI sources)\n");
  return 0;
}

#else

#include "core/Configuration.h"
#include "core/IniStorage.h"
#include "core/SessionData.h"
#include "gui/AuthenticateDialog.h"
#include "gui/GuiUserInterface.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <cstdio>
#include <functional>

static int Failures = 0;
static int Checks = 0;

#define CHECK(Cond) \
  do { Checks++; if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_MSG(Cond, Msg) \
  do { Checks++; if (!(Cond)) { std::printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, #Cond, qPrintable(Msg)); std::fflush(stdout); Failures++; } } while (0)

static QString ScreenshotDir;

static void Screenshot(QWidget * Widget, const QString & Name)
{
  if (!ScreenshotDir.isEmpty())
  {
    Widget->grab().save(QDir(ScreenshotDir).filePath(Name + QStringLiteral(".png")));
  }
}

static QStringList Captions(QMenu * Menu)
{
  QStringList Result;
  if (Menu != nullptr)
  {
    for (QAction * Action : Menu->actions())
    {
      Result.append(Action->text());
    }
  }
  return Result;
}

// Runs Call in a worker thread (as the core calls the UserInterface) while
// Interact is polled from the GUI thread (inside the nested event loop of the
// dialog) until it returns true.
static bool RunWorker(const std::function<void()> & Call, const std::function<bool()> & Interact, int Ms = 15000)
{
  std::atomic<bool> Done{false};
  QThread * Worker = QThread::create([&]() { Call(); Done = true; });
  bool Interacted = !Interact;
  QTimer Poll;
  Poll.setInterval(30);
  QObject::connect(&Poll, &QTimer::timeout, [&]()
  {
    if (!Interacted && Interact())
    {
      Interacted = true;
    }
  });
  Poll.start();
  Worker->start();
  QElapsedTimer Timer;
  Timer.start();
  while (!Done && (Timer.elapsed() < Ms))
  {
    QApplication::processEvents(QEventLoop::AllEvents, 10);
    QThread::msleep(2);
  }
  Poll.stop();
  bool Result = Done;
  if (!Done)
  {
    // Unblock: close whatever is modal
    for (int I = 0; (I < 50) && !Done; I++)
    {
      if (QWidget * Modal = QApplication::activeModalWidget())
      {
        if (auto * Dialog = qobject_cast<QDialog *>(Modal)) Dialog->reject();
      }
      QApplication::processEvents(QEventLoop::AllEvents, 50);
    }
  }
  Worker->wait(5000);
  delete Worker;
  return Result && Interacted;
}

static MessageForm * ActiveMessageForm()
{
  for (QWidget * Widget : QApplication::topLevelWidgets())
  {
    if (auto * Form = qobject_cast<MessageForm *>(Widget))
    {
      if (Form->isVisible()) return Form;
    }
  }
  return nullptr;
}

// TQueryParams of the overwrite prompt (Terminal::ConfirmOverwrite)
static MessageParams OverwriteParams()
{
  QueryUserParams Q;
  Q.Params = qpNeverAskAgainCheck;
  Q.HelpKeyword = QStringLiteral("ui_overwrite");
  Q.Aliases.append(QueryButtonAlias{qaRetry, QStringLiteral("A&ppend"), qaNo, Qt::AltModifier});
  Q.Aliases.append(QueryButtonAlias::AllAsYesToNewerGroupedWithYes());
  Q.Aliases.append(QueryButtonAlias::IgnoreAsRenameGroupedWithNo());
  Q.Aliases.append(QueryButtonAlias::YesToAllGroupedWithYes());
  Q.Aliases.append(QueryButtonAlias::NoToAllGroupedWithNo());
  return MessageParams::From(Q);
}
static const unsigned int OverwriteAnswers = qaYes | qaNo | qaCancel | qaYesToAll | qaNoToAll | qaRetry | qaAll | qaIgnore;

static void TestButtonsAndGroups(QWidget * Parent)
{
  MessageParams Params = OverwriteParams();
  MessageForm Form(Parent, QStringLiteral("**Overwrite local file 'a.txt'?**\n\nDestination directory already contains file 'a.txt'."),
    QString(), qtConfirmation, OverwriteAnswers, QStringLiteral("ui_overwrite"), &Params);
  Form.show();
  QApplication::processEvents();
  Screenshot(&Form, QStringLiteral("overwrite"));

  CHECK(Form.windowTitle() == QStringLiteral("Confirm"));
  CHECK(Form.MainInstructionLabel() != nullptr);
  CHECK((Form.MainInstructionLabel() != nullptr) && (Form.MainInstructionLabel()->text() == QStringLiteral("Overwrite local file 'a.txt'?")));
  CHECK(Form.MessageLabel()->text() == QStringLiteral("Destination directory already contains file 'a.txt'."));
  // Own buttons: Yes, No, Cancel, Help; the rest are drop-down items
  CHECK(Form.AnswerButton(qaYes) != nullptr);
  CHECK(Form.AnswerButton(qaNo) != nullptr);
  CHECK(Form.AnswerButton(qaCancel) != nullptr);
  CHECK(Form.AnswerButton(qaHelp) != nullptr);
  CHECK(Form.AnswerButton(qaYesToAll) == nullptr);
  CHECK(Form.AnswerButton(qaAll) == nullptr);
  CHECK(Form.AnswerButton(qaRetry) == nullptr);
  QStringList YesItems = Captions(Form.DropDownMenu(qaYes));
  CHECK_MSG(YesItems == (QStringList{QStringLiteral("&Yes"), QStringLiteral("Yes to A&ll\tShift"), QStringLiteral("Ne&wer only\tCtrl")}),
    YesItems.join(QLatin1Char('|')));
  QStringList NoItems = Captions(Form.DropDownMenu(qaNo));
  CHECK_MSG(NoItems == (QStringList{QStringLiteral("&No"), QStringLiteral("N&o to All\tShift"), QStringLiteral("A&ppend\tAlt"),
    QStringLiteral("New na&me\tCtrl")}), NoItems.join(QLatin1Char('|')));
  CHECK(static_cast<QPushButton *>(Form.AnswerButton(qaYes))->isDefault());

  // Modifiers switch the split buttons (UpdateForShiftState)
  Form.UpdateForModifiers(Qt::ShiftModifier);
  CHECK(Form.AnswerButton(qaYes)->text() == QStringLiteral("Yes to A&ll"));
  CHECK(Form.ButtonAnswer(Form.AnswerButton(qaYes)) == qaYesToAll);
  CHECK(Form.ButtonAnswer(Form.AnswerButton(qaNo)) == qaNoToAll);
  Form.UpdateForModifiers(Qt::ControlModifier);
  CHECK(Form.ButtonAnswer(Form.AnswerButton(qaYes)) == qaAll);
  CHECK(Form.ButtonAnswer(Form.AnswerButton(qaNo)) == qaIgnore);
  CHECK(Form.AnswerButton(qaNo)->text() == QStringLiteral("New na&me"));
  Form.UpdateForModifiers(Qt::AltModifier);
  CHECK(Form.ButtonAnswer(Form.AnswerButton(qaNo)) == qaRetry);
  CHECK(Form.ButtonAnswer(Form.AnswerButton(qaYes)) == qaYes);
  Form.UpdateForModifiers(Qt::NoModifier);
  CHECK(Form.AnswerButton(qaYes)->text() == QStringLiteral("&Yes"));
  CHECK(Form.ButtonAnswer(Form.AnswerButton(qaNo)) == qaNo);

  // Shift held while clicking Yes = Yes to All
  Form.UpdateForModifiers(Qt::ShiftModifier);
  Form.AnswerButton(qaYes)->click();
  CHECK(Form.Answer() == qaYesToAll);

  // Ctrl+C text
  CHECK(Form.FormText().contains(QStringLiteral("Overwrite local file 'a.txt'?")));
  CHECK(Form.FormText().startsWith(QStringLiteral("---------------------------\nConfirm\n")));
}

static void TestNeverAskAgain(QWidget * Parent)
{
  {
    MessageParams Params = OverwriteParams();
    MessageForm Form(Parent, QStringLiteral("q"), QString(), qtConfirmation, OverwriteAnswers, QStringLiteral("ui_overwrite"), &Params);
    Form.show();
    QCheckBox * Check = Form.NeverAskAgainCheck();
    CHECK(Check != nullptr);
    CHECK((Check != nullptr) && (Check->text() == QStringLiteral("Never ask me a&gain")));
    if (Check == nullptr) return;
    Check->setChecked(true);
    // Only the positive answer and Cancel stay enabled; drop-down items are disabled
    CHECK(Form.AnswerButton(qaYes)->isEnabled());
    CHECK(!Form.AnswerButton(qaNo)->isEnabled());
    CHECK(Form.AnswerButton(qaCancel)->isEnabled());
    CHECK(Form.AnswerButton(qaHelp)->isEnabled());
    QList<QAction *> Items = Form.DropDownMenu(qaYes)->actions();
    CHECK(Items.value(0)->isEnabled());
    CHECK(!Items.value(1)->isEnabled());
    Form.UpdateForModifiers(Qt::ShiftModifier);
    CHECK(Form.ButtonAnswer(Form.AnswerButton(qaYes)) == qaYes);   // Yes to All disabled
    Form.UpdateForModifiers(Qt::NoModifier);
    Form.AnswerButton(qaYes)->click();
    CHECK(Form.Answer() == qaNeverAskAgain);
  }
  {
    // Unchecked again: plain answer
    MessageParams Params;
    Params.Params = qpNeverAskAgainCheck;
    MessageForm Form(Parent, QStringLiteral("q"), QString(), qtConfirmation, qaYes | qaNo, QString(), &Params);
    Form.show();
    Form.NeverAskAgainCheck()->setChecked(true);
    Form.NeverAskAgainCheck()->setChecked(false);
    CHECK(Form.AnswerButton(qaNo)->isEnabled());
    Form.AnswerButton(qaNo)->click();
    CHECK(Form.Answer() == qaNo);
  }
  {
    // Checked + Cancel: Cancel, not "never ask again"
    MessageParams Params;
    Params.Params = qpNeverAskAgainCheck;
    MessageForm Form(Parent, QStringLiteral("q"), QString(), qtConfirmation, qaYes | qaNo | qaCancel, QString(), &Params);
    Form.show();
    Form.NeverAskAgainCheck()->setChecked(true);
    Form.reject();
    CHECK(Form.Answer() == qaCancel);
  }
  {
    // OK only: NEVER_SHOW_AGAIN caption
    MessageParams Params;
    Params.Params = qpNeverAskAgainCheck;
    MessageForm Form(Parent, QStringLiteral("q"), QString(), qtInformation, qaOK, QString(), &Params);
    CHECK(Form.NeverAskAgainCheck()->text() == QStringLiteral("Never show this message a&gain"));
    CHECK(Form.windowTitle() == QStringLiteral("Information"));
  }
  {
    // No check box without qpNeverAskAgainCheck; Escape = cancel answer (No)
    MessageForm Form(Parent, QStringLiteral("q"), QString(), qtWarning, qaYes | qaNo, QString(), nullptr);
    Form.show();
    CHECK(Form.NeverAskAgainCheck() == nullptr);
    CHECK(Form.windowTitle() == QStringLiteral("Warning"));
    Form.reject();
    CHECK(Form.Answer() == qaNo);
  }
}

static void TestCountdown(QWidget * Parent)
{
  {
    MessageParams Params;
    Params.Timeout = 3000;
    Params.TimeoutAnswer = qaRetry;
    MessageButtonAlias ReconnectAlias;
    ReconnectAlias.Button = qaRetry;
    ReconnectAlias.Alias = QStringLiteral("&Reconnect");
    Params.Aliases.append(ReconnectAlias);
    MessageForm Form(Parent, QStringLiteral("**Connection has been unexpectedly closed.**"), QStringLiteral("details"), qtError,
      qaRetry | qaOK, QString(), &Params);
    Form.show();
    QApplication::processEvents();
    Screenshot(&Form, QStringLiteral("countdown"));
    CHECK(Form.windowTitle() == QStringLiteral("Error"));
    QPushButton * Button = Form.TimeoutButton();
    CHECK(Button != nullptr);
    if (Button == nullptr) return;
    CHECK_MSG(Button->text() == QStringLiteral("&Reconnect (3 s)"), Button->text());
    Form.TimeoutTick();
    CHECK_MSG(Button->text() == QStringLiteral("&Reconnect (2 s)"), Button->text());
    Form.SuspendTimeout();
    CHECK_MSG(Button->text() == QStringLiteral("&Reconnect (30 s)"), Button->text());
    Form.CancelTimeout();
    CHECK_MSG(Button->text() == QStringLiteral("&Reconnect"), Button->text());
    Form.TimeoutTick();
    CHECK(Form.isVisible());
    Form.reject();
    CHECK(Form.Answer() == qaOK);
  }
  {
    // Expiry answers automatically
    MessageParams Params;
    Params.Timeout = 2000;
    Params.TimeoutAnswer = qaRetry;
    MessageForm Form(Parent, QStringLiteral("q"), QString(), qtError, qaRetry | qaAbort, QString(), &Params);
    Form.show();
    Form.TimeoutTick();
    CHECK(Form.TimeoutButton()->text() == QStringLiteral("&Retry (1 s)"));
    Form.TimeoutTick();
    CHECK(!Form.isVisible());
    CHECK(Form.Answer() == qaRetry);
  }
  {
    // Real timer through Execute (1 s)
    MessageParams Params;
    Params.Timeout = 1000;
    Params.TimeoutAnswer = qaOK;
    QElapsedTimer Timer;
    Timer.start();
    unsigned int Answer = MessageDialog(Parent, QStringLiteral("auto"), qtInformation, qaOK | qaCancel, QString(), &Params);
    CHECK(Answer == qaOK);
    CHECK(Timer.elapsed() < 5000);
  }
}

static void TestHelpAndSubmit(QWidget * Parent)
{
  QList<QUrl> Opened;
  SetOpenUrlHandler([&Opened](const QUrl & Url) { Opened.append(Url); });
  {
    MessageForm Form(Parent, QStringLiteral("q"), QString(), qtConfirmation, qaOK, QStringLiteral("ui_overwrite"), nullptr);
    Form.show();
    CHECK(Form.AnswerButton(qaHelp) != nullptr);
    CHECK(Form.AnswerButton(qaHelp)->text() == QStringLiteral("&Help"));
    Form.AnswerButton(qaHelp)->click();
    CHECK(Form.isVisible());
    CHECK(Opened.value(0) == QUrl(QStringLiteral("https://winscp.net/eng/docs/ui_overwrite")));
  }
  {
    MessageParams Params;
    Params.AllowHelp = false;
    MessageForm Form(Parent, QStringLiteral("q"), QString(), qtConfirmation, qaOK, QString(), &Params);
    CHECK(Form.AnswerButton(qaHelp) == nullptr);
  }
  SetOpenUrlHandler(nullptr);

  // OnSubmit aliases and the action link
  int Copied = 0;
  bool PasteMatches = false;
  MessageParams Params;
  MessageButtonAlias Copy;
  Copy.Button = qaRetry;
  Copy.Alias = QStringLiteral("&Copy Key");
  Copy.ActionAlias = QStringLiteral("&Copy key fingerprints to clipboard");
  Copy.OnSubmit = [&Copied]() -> unsigned int { Copied++; return 0; };
  Params.Aliases.append(Copy);
  MessageButtonAlias Paste;
  Paste.Button = qaIgnore;
  Paste.Alias = QStringLiteral("&Paste key");
  Paste.GroupWith = qaYes;
  Paste.OnSubmit = [&PasteMatches]() -> unsigned int { return PasteMatches ? qaYes : 0; };
  Params.Aliases.append(Paste);
  MessageForm Form(Parent, QStringLiteral("q"), QString(), qtWarning, qaYes | qaCancel | qaRetry | qaIgnore, QString(), &Params);
  Form.show();
  CHECK(Form.LinkLabel() != nullptr);
  CHECK(Form.AnswerButton(qaRetry) == nullptr);
  CHECK(Captions(Form.DropDownMenu(qaYes)).contains(QStringLiteral("&Paste key")));
  Form.SubmitAnswer(qaRetry);
  CHECK(Copied == 1);
  CHECK(Form.isVisible());
  Form.SubmitAnswer(qaIgnore);
  CHECK(Form.isVisible());
  PasteMatches = true;
  Form.SubmitAnswer(qaIgnore);
  CHECK(!Form.isVisible());
  CHECK(Form.Answer() == qaYes);
}

static void TestHostKeyMatch()
{
  QByteArray Blob = QByteArray::fromHex("0000000b7373682d6564323535313900000020") + QByteArray(32, '\x42');
  QString Sha = QString::fromLatin1(QCryptographicHash::hash(Blob, QCryptographicHash::Sha256).toBase64(QByteArray::OmitTrailingEquals));
  QString Fp256 = QStringLiteral("SHA256:") + Sha;
  QString FpMD5 = QStringLiteral("MD5:aa:bb:cc:dd:ee:ff:00:11:22:33:44:55:66:77:88:99");
  QString Type = QStringLiteral("ssh-ed25519");
  CHECK(HostKeyMatchesText(Fp256, Type, Fp256, FpMD5));
  CHECK(HostKeyMatchesText(Sha, Type, Fp256, FpMD5));
  CHECK(HostKeyMatchesText(Sha + QStringLiteral("="), Type, Fp256, FpMD5));
  CHECK(HostKeyMatchesText(QStringLiteral("ssh-ed25519 255 ") + Sha, Type, Fp256, FpMD5));
  CHECK(HostKeyMatchesText(QStringLiteral("  ssh-ed25519 255 SHA256:") + Sha + QStringLiteral("\n"), Type, Fp256, FpMD5));
  CHECK(HostKeyMatchesText(QStringLiteral("AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99"), Type, Fp256, FpMD5));
  CHECK(HostKeyMatchesText(QStringLiteral("ssh-ed25519 255 aa:bb:cc:dd:ee:ff:00:11:22:33:44:55:66:77:88:99"), Type, Fp256, FpMD5));
  CHECK(HostKeyMatchesText(QStringLiteral("ssh-ed25519 ") + QString::fromLatin1(Blob.toBase64()) + QStringLiteral(" user@host"), Type, Fp256, FpMD5));
  CHECK(!HostKeyMatchesText(QStringLiteral("ssh-ed25519 ") + QString::fromLatin1((Blob + "x").toBase64()), Type, Fp256, FpMD5));
  CHECK(!HostKeyMatchesText(QStringLiteral("SHA256:abc"), Type, Fp256, FpMD5));
  CHECK(!HostKeyMatchesText(QString(), Type, Fp256, FpMD5));
  CHECK(!HostKeyMatchesText(QStringLiteral("aa:bb"), Type, Fp256, FpMD5));
}

static void TestWorkerQueries(GuiUserInterface & UI)
{
  // QueryUserEx from a worker thread with qpNeverAskAgainCheck returns
  // qaNeverAskAgain (the dead Terminal.cpp branch of WP19)
  unsigned int Result = 0;
  QueryUserParams Q;
  Q.Params = qpNeverAskAgainCheck | qpAllowContinueOnError;
  Q.Type = qtConfirmation;
  bool Ok = RunWorker([&]() {
      Result = UI.QueryUserEx(QStringLiteral("**Do you want to resume file transfer?**"), QString(), qaYes | qaNo | qaCancel, Q);
    },
    [&]() -> bool
    {
      MessageForm * Form = ActiveMessageForm();
      if (Form == nullptr) return false;
      Form->NeverAskAgainCheck()->setChecked(true);
      Form->AnswerButton(qaYes)->click();
      return true;
    });
  CHECK(Ok);
  CHECK(Result == qaNeverAskAgain);

  // Plain QueryUser goes through the same form
  Ok = RunWorker([&]() { Result = UI.QueryUser(QStringLiteral("q"), QStringLiteral("more"), qaRetry | qaSkip | qaAbort); },
    [&]() -> bool
    {
      MessageForm * Form = ActiveMessageForm();
      if (Form == nullptr) return false;
      CHECK(Form->NeverAskAgainCheck() == nullptr);
      CHECK(Form->AnswerButton(qaSkip) != nullptr);
      Form->AnswerButton(qaSkip)->click();
      return true;
    });
  CHECK(Ok);
  CHECK(Result == qaSkip);
}

static void TestHostKeyPrompt(GuiUserInterface & UI)
{
  QByteArray Blob = QByteArray::fromHex("0000000b7373682d6564323535313900000020") + QByteArray(32, '\x07');
  QString Fp256 = QStringLiteral("SHA256:") +
    QString::fromLatin1(QCryptographicHash::hash(Blob, QCryptographicHash::Sha256).toBase64(QByteArray::OmitTrailingEquals));
  QString FpMD5 = QStringLiteral("MD5:01:02:03:04:05:06:07:08:09:0a:0b:0c:0d:0e:0f:10");
  UserInterface::HostKeyAnswer Answer = UserInterface::HostKeyCancel;
  bool Ok = RunWorker([&]() {
      Answer = UI.VerifyHostKey(QStringLiteral("example.com"), 22, QStringLiteral("ssh-ed25519"), Fp256, FpMD5, UserInterface::HostKeyUnknown);
    },
    [&]() -> bool
    {
      MessageForm * Form = ActiveMessageForm();
      if (Form == nullptr) return false;
      Screenshot(Form, QStringLiteral("hostkey"));
      CHECK(Form->windowTitle() == QStringLiteral("Warning"));
      CHECK(Form->AnswerButton(qaYes) && (Form->AnswerButton(qaYes)->text() == QStringLiteral("&Accept")));
      QStringList Items = Captions(Form->DropDownMenu(qaYes));
      CHECK_MSG(Items == (QStringList{QStringLiteral("&Accept"), QStringLiteral("Connect &Once"), QStringLiteral("&Paste key")}),
        Items.join(QLatin1Char('|')));
      CHECK(Form->LinkLabel() != nullptr);
      CHECK(Form->MessageLabel()->text().contains(QStringLiteral("example.com (port 22)")));
      CHECK(Form->MessageLabel()->text().contains(QStringLiteral("ssh-ed25519") + QChar(0xA0) + QStringLiteral("255")));
      // Copy key
      Form->SubmitAnswer(qaRetry);
      CHECK(QApplication::clipboard()->text().startsWith(QStringLiteral("ssh-ed25519 255 ")));
      CHECK(Form->isVisible());
      Form->SubmitAnswer(qaOK);
      return true;
    });
  CHECK(Ok);
  CHECK(Answer == UserInterface::HostKeyOnce);

  // Paste key with the public key line in the clipboard = Accept
  QApplication::clipboard()->setText(QStringLiteral("ssh-ed25519 ") + QString::fromLatin1(Blob.toBase64()) + QStringLiteral(" root@example"));
  Ok = RunWorker([&]() {
      Answer = UI.VerifyHostKey(QStringLiteral("example.com"), 2222, QStringLiteral("ssh-ed25519"), Fp256, FpMD5, UserInterface::HostKeyChanged);
    },
    [&]() -> bool
    {
      MessageForm * Form = ActiveMessageForm();
      if (Form == nullptr) return false;
      CHECK(Form->AnswerButton(qaYes) && (Form->AnswerButton(qaYes)->text() == QStringLiteral("&Update")));
      CHECK(Form->MainInstructionLabel() && Form->MainInstructionLabel()->text().contains(QStringLiteral("Potential security breach")));
      CHECK(Form->AnswerButton(qaCancel) != nullptr);
      Form->SubmitAnswer(qaIgnore);
      return true;
    });
  CHECK(Ok);
  CHECK(Answer == UserInterface::HostKeyAccept);
}

static void TestAuthenticate(GuiUserInterface & UI)
{
  Configuration & Config = Configuration::Instance();
  Config.SessionRememberPassword = false;
  Config.PuttyPassword = false;

  SessionData Data;
  Data.HostName = QStringLiteral("auth.example.com");
  Data.UserName = QStringLiteral("martin");
  Data.PortNumber = 2222;
  Data.FSProtocol = fsSFTP;
  bool Canceled = false;
  UI.BeginConnecting(Data, [&Canceled]() { Canceled = true; });
  AuthenticateDialog * Dialog = UI.Authenticate();
  CHECK(Dialog != nullptr);
  if (Dialog == nullptr) return;
  QApplication::processEvents();
  CHECK(Dialog->isVisible());
  CHECK(Dialog->windowTitle() == Data.SessionName());
  CHECK(Dialog->LogLines() == (QStringList{QStringLiteral("Searching for host..."), QStringLiteral("Connecting to host...")}));
  UI.Information(QStringLiteral("Custom line"));
  CHECK(Dialog->LogLines().last() == QStringLiteral("Custom line"));

  // Password prompt in the same window, with "Remember password for this session"
  QList<Prompt> Prompts{Prompt{QStringLiteral("&Password: "), false, QString()}};
  bool Result = false;
  bool Ok = RunWorker([&]() { Result = UI.PromptUser(UserInterface::pkPassword, Data.SessionName(), QString(), Prompts); },
    [&]() -> bool
    {
      if (!Dialog->Busy()) return false;
      Screenshot(Dialog, QStringLiteral("authenticate-password"));
      CHECK(Dialog->windowTitle() == QStringLiteral("Password - ") + Data.SessionName());
      CHECK(Dialog->PasswordPanel()->isVisible());
      CHECK(Dialog->LogView()->isVisible());
      CHECK(Dialog->SessionRememberPasswordCheck()->isVisible());
      CHECK(!Dialog->SavePasswordCheck()->isVisible());
      CHECK(Dialog->PromptEdits().size() == 1);
      if (Dialog->PromptEdits().isEmpty()) return false;
      CHECK(Dialog->PromptEdits().first()->echoMode() == QLineEdit::Password);
      Dialog->PromptEdits().first()->setText(QStringLiteral("secret1"));
      Dialog->SessionRememberPasswordCheck()->setChecked(true);
      Dialog->PasswordOKButton()->click();
      return true;
    });
  CHECK(Ok);
  CHECK(Result);
  CHECK(Prompts[0].Result == QStringLiteral("secret1"));
  CHECK(UI.TakeSessionRememberPassword());
  CHECK(!UI.TakeSessionRememberPassword());
  CHECK(UI.TakeLastPassword() == QStringLiteral("secret1"));
  QStringList Lines = Dialog->LogLines();
  CHECK_MSG(Lines.contains(QStringLiteral("Authenticating...")), Lines.join(QLatin1Char('|')));
  CHECK(Lines.contains(QStringLiteral("Using username \"martin\".")));
  CHECK(Dialog->windowTitle() == Data.SessionName());
  CHECK(!Dialog->PasswordPanel()->isVisible());

  // Keyboard-interactive with two prompts, Cancel
  QList<Prompt> KI{Prompt{QStringLiteral("Code:"), true, QString()}, Prompt{QStringLiteral("PIN:"), false, QString()}};
  Ok = RunWorker([&]() { Result = UI.PromptUser(UserInterface::pkKeybInteractive, Data.SessionName(), QStringLiteral("Visit https://example.com/otp now"), KI); },
    [&]() -> bool
    {
      if (!Dialog->Busy()) return false;
      CHECK(Dialog->PromptEdits().size() == 2);
      CHECK(!Dialog->SessionRememberPasswordCheck()->isVisible());
      CHECK(Dialog->windowTitle().startsWith(QStringLiteral("Server prompt")));
      Dialog->PasswordCancelButton()->click();
      return true;
    });
  CHECK(Ok);
  CHECK(!Result);
  CHECK(!Canceled);

  // Banner: never show again + monospaced font, stored in Configuration\Banners
  QString BannerText = QStringLiteral("Welcome to the test server\nAuthorized use only");
  Ok = RunWorker([&]() { UI.DisplayBanner(Data.SessionName(), BannerText); },
    [&]() -> bool
    {
      if (!Dialog->Busy()) return false;
      Screenshot(Dialog, QStringLiteral("authenticate-banner"));
      CHECK(Dialog->windowTitle() == QStringLiteral("Authentication Banner - ") + Data.SessionName());
      CHECK(Dialog->BannerMemo()->toPlainText() == BannerText);
      CHECK(Dialog->NeverShowAgainCheck()->isVisible());
      CHECK(!Dialog->LogView()->isVisible());
      CHECK(!Dialog->BannerMonospacedFontAction()->isChecked());
      Dialog->BannerMonospacedFontAction()->trigger();
      Dialog->NeverShowAgainCheck()->setChecked(true);
      Dialog->BannerCloseButton()->click();
      return true;
    });
  CHECK(Ok);
  QByteArray Utf16(reinterpret_cast<const char *>(BannerText.utf16()), BannerText.size() * 2);
  QString ExpectedHash = QString::fromLatin1(QCryptographicHash::hash(Utf16, QCryptographicHash::Md5).toHex().toUpper());
  CHECK(GuiUserInterface::BannerHash(BannerText) == ExpectedHash);
  CHECK(GuiUserInterface::SessionKey(Data) == QStringLiteral("martin@auth.example.com:2222"));
  {
    IniStorage Storage(&Config.Ini());
    CHECK(Storage.OpenSubKeyPath(QStringLiteral("Configuration\\Banners"), false));
    QString Value = Storage.ReadString(QStringLiteral("martin@auth.example.com:2222"), QString());
    CHECK_MSG(Value == ExpectedHash + QStringLiteral(",1"), Value);
  }
  // Not shown again
  Ok = RunWorker([&]() { UI.DisplayBanner(Data.SessionName(), BannerText); }, nullptr, 5000);
  CHECK(Ok);
  CHECK(!Dialog->Busy());

  // Closing the window while it shows the log only cancels the connecting
  Dialog->close();
  CHECK(Canceled);
  UI.EndConnecting();
  CHECK(UI.Authenticate() == nullptr);
  QApplication::processEvents();

  // Change stored password to this one
  auto Site = std::make_shared<SessionData>(Data);
  Site->Name = QStringLiteral("Stored site");
  Site->Password = QStringLiteral("oldpass");
  Config.StoredSessions().Add(Site);
  Config.Save();
  SessionData Stored = *Site;
  UI.BeginConnecting(Stored);
  Dialog = UI.Authenticate();
  QList<Prompt> P2{Prompt{QStringLiteral("&Password: "), false, QString()}};
  Ok = RunWorker([&]() { Result = UI.PromptUser(UserInterface::pkPassword, Stored.SessionName(), QStringLiteral("Access denied."), P2); },
    [&]() -> bool
    {
      if ((Dialog == nullptr) || !Dialog->Busy()) return false;
      CHECK(Dialog->SavePasswordCheck()->isVisible());
      CHECK(!Dialog->SessionRememberPasswordCheck()->isVisible());
      Dialog->PromptEdits().first()->setText(QStringLiteral("newpass"));
      Dialog->SavePasswordCheck()->setChecked(true);
      Dialog->PasswordOKButton()->click();
      return true;
    });
  CHECK(Ok);
  CHECK(Result);
  auto Found = Config.StoredSessions().FindByName(QStringLiteral("Stored site"));
  CHECK((Found != nullptr) && (Found->Password == QStringLiteral("newpass")));
  UI.EndConnecting();

  // A prompt of another session (no Authenticate window): its own window
  QList<Prompt> P3{Prompt{QStringLiteral("&Username:"), true, QString()}};
  Ok = RunWorker([&]() { Result = UI.PromptUser(UserInterface::pkUserName, QStringLiteral("other@host"), QString(), P3); },
    [&]() -> bool
    {
      for (QWidget * Widget : QApplication::topLevelWidgets())
      {
        auto * Own = qobject_cast<AuthenticateDialog *>(Widget);
        if ((Own != nullptr) && Own->Busy())
        {
          CHECK(Own->windowTitle() == QStringLiteral("Username - other@host"));
          CHECK(!Own->LogView()->isVisible());
          Own->PromptEdits().first()->setText(QStringLiteral("joe"));
          Own->PasswordOKButton()->click();
          return true;
        }
      }
      return false;
    });
  CHECK(Ok);
  CHECK(Result && (P3[0].Result == QStringLiteral("joe")));
  Config.SessionRememberPassword = true;
}

int main(int argc, char ** argv)
{
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
  {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  QStandardPaths::setTestModeEnabled(true);
  QApplication App(argc, argv);
  QTemporaryDir Scratch;
  Configuration::Instance().SetIniFileName(Scratch.filePath(QStringLiteral("nixscp.ini")));
  Configuration::Instance().Load();
  ScreenshotDir = qEnvironmentVariable("NIXSCP_TEST_SCREENSHOT");

  QMainWindow Window;
  Window.resize(800, 600);
  Window.show();
  GuiUserInterface UI(&Window);

  TestButtonsAndGroups(&Window);
  TestNeverAskAgain(&Window);
  TestCountdown(&Window);
  TestHelpAndSubmit(&Window);
  TestHostKeyMatch();
  TestWorkerQueries(UI);
  TestHostKeyPrompt(UI);
  TestAuthenticate(UI);

  std::printf("%d checks, %d failures\n", Checks, Failures);
  return (Failures == 0) ? 0 : 1;
}

#endif
