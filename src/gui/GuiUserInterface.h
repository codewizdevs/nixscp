// Port of WinSCP source/windows/WinInterface.cpp (MessageDialog, MoreMessageDialog,
// countdown TMessageTimeout, "Never ask me again"), source/forms/MessageDlg.cpp
// (TMessageForm: button aliases, grouped split buttons, Help button),
// the prompt part of source/windows/UserInterface.cpp / TTerminalManager
// (Authenticate window, banner) and the host key prompt of
// source/core/SecureShell.cpp (VerifyHostKey).
#pragma once

#include "core/FileSystem.h"

#include <QDialog>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QUrl>
#include <QWidget>

#include <functional>
#include <memory>

class AuthenticateDialog;
class QAbstractButton;
class QCheckBox;
class QLabel;
class QMenu;
class QPushButton;
class QTimer;
class QToolButton;
class SessionData;

// TQueryButtonAlias as the GUI knows it: QueryButtonAlias plus OnSubmit
// (the button does not close the dialog by itself; OnSubmit returns the answer
// to close it with, or 0 to keep it open) and ActionAlias (shown as a link in
// the message area instead of a button)
struct MessageButtonAlias
{
  unsigned int Button = 0;
  QString Alias;
  unsigned int GroupWith = 0;
  Qt::KeyboardModifiers GroupedModifiers = Qt::NoModifier;
  std::function<unsigned int()> OnSubmit;
  QString ActionAlias;

  static MessageButtonAlias From(const QueryButtonAlias & Alias);
};

// TMessageParams
struct MessageParams
{
  unsigned int Params = 0;                 // qpNeverAskAgainCheck
  QList<MessageButtonAlias> Aliases;
  unsigned int Timeout = 0;                // ms, countdown on the TimeoutAnswer button
  unsigned int TimeoutAnswer = 0;
  QString NeverAskAgainTitle;              // default NEVER_ASK_AGAIN / NEVER_SHOW_AGAIN
  unsigned int NeverAskAgainAnswer = 0;    // 0 = any positive answer
  bool NeverAskAgainCheckedInitially = false;
  bool AllowHelp = true;
  QString CustomCaption;

  static MessageParams From(const QueryUserParams & Params);
};

// TMessageForm: icon by query type, "**main instruction**" markup, more
// messages memo, answer buttons in WinSCP order with aliases and split
// buttons (grouped answers in the drop-down, chosen directly with the
// grouped modifier held: Shift+Yes = Yes to All, Ctrl+Yes = Newer only...),
// "Never ask me again" check box, countdown, Help button, Ctrl+C copies the text.
class MessageForm : public QDialog
{
  Q_OBJECT

public:
  MessageForm(QWidget * Parent, const QString & Message, const QString & MoreMessages, QueryType Type,
    unsigned int Answers, const QString & HelpKeyword, const MessageParams * Params);
  ~MessageForm() override;

  // ShowModal + ExecuteMessageDialog: the answer, qaNeverAskAgain when the
  // check box is checked and the answer is positive
  unsigned int Execute();
  // Answer of the closed form (as Execute returns; for non-modal use)
  unsigned int Answer() const;

  // For tests and automation
  QAbstractButton * AnswerButton(unsigned int Answer) const;   // own button (not grouped)
  unsigned int ButtonAnswer(QAbstractButton * Button) const;   // current answer (after modifiers)
  QMenu * DropDownMenu(unsigned int Answer) const;             // menu of a split button
  QCheckBox * NeverAskAgainCheck() const { return FNeverAskAgainCheck; }
  QLabel * MainInstructionLabel() const { return FMainLabel; }
  QLabel * MessageLabel() const { return FMessageLabel; }
  QLabel * LinkLabel() const { return FLinkLabel; }
  QPushButton * TimeoutButton() const;
  void UpdateForModifiers(Qt::KeyboardModifiers Modifiers);
  void SubmitAnswer(unsigned int Answer);                       // as clicking the answer's button/item
  QString FormText() const;                                     // GetFormText (Ctrl+C)
  // Moves the countdown on (TMessageTimeout::DoTimer); Ms elapsed
  void TimeoutTick();
  // A click or a key press cancels the countdown, moving the mouse suspends it
  void CancelTimeout();
  void SuspendTimeout();
  // mrCancel (X button, Escape): the valid "cancel" answer
  void reject() override;

protected:
  void keyPressEvent(QKeyEvent * Event) override;
  void keyReleaseEvent(QKeyEvent * Event) override;
  bool eventFilter(QObject * Watched, QEvent * Event) override;

private:
  struct ButtonInfo
  {
    QPushButton * Button = nullptr;
    QToolButton * DropDown = nullptr;
    QMenu * Menu = nullptr;
    unsigned int OwnAnswer = 0;
    unsigned int CurrentAnswer = 0;
    QString OwnCaption;
  };

  void ButtonClicked(ButtonInfo * Info);
  void MenuItemClicked(QAction * Action);
  void NeverAskAgainCheckClick();
  void UpdateTimeoutButton();
  void HelpButtonSubmit();
  void Finish(unsigned int Answer);
  const MessageButtonAlias * FindAlias(unsigned int Answer) const;
  ButtonInfo * InfoOf(QAbstractButton * Button) const;

  QString FMessage;
  QString FMoreMessages;
  QString FHelpKeyword;
  unsigned int FAnswers;
  unsigned int FCancelAnswer;
  MessageParams FParams;
  QList<ButtonInfo *> FButtons;
  QCheckBox * FNeverAskAgainCheck = nullptr;
  QLabel * FMainLabel = nullptr;
  QLabel * FMessageLabel = nullptr;
  QLabel * FLinkLabel = nullptr;
  unsigned int FAnswer = 0;
  Qt::KeyboardModifiers FModifiers = Qt::NoModifier;
  // Countdown (TMessageTimeoutButton)
  ButtonInfo * FTimeoutInfo = nullptr;
  QTimer * FTimeoutTimer = nullptr;
  int FOrigTimeout = 0;
  int FTimeout = 0;
  QPoint FOrigCursorPos;
};

// MoreMessageDialog / MessageDialog (windows/WinInterface.cpp)
unsigned int MoreMessageDialog(QWidget * Parent, const QString & Message, const QString & MoreMessages, QueryType Type,
  unsigned int Answers, const QString & HelpKeyword = QString(), const MessageParams * Params = nullptr);
unsigned int MessageDialog(QWidget * Parent, const QString & Message, QueryType Type, unsigned int Answers,
  const QString & HelpKeyword = QString(), const MessageParams * Params = nullptr);

// Shows an error the way WinSCP's MessageDialog does: message + more messages memo, OK + Help
void ShowErrorMessage(QWidget * Parent, const QString & Message, const QString & MoreMessages = QString());

// FormHelp / TWebHelpSystem: opens https://winscp.net/eng/docs/<keyword>;
// without a keyword offers to search the documentation for Message (HELP_SEND_MESSAGE2)
void OpenHelp(const QString & HelpKeyword, QWidget * Parent = nullptr, const QString & Message = QString());
// Opens Url in the desktop browser (or passes it to the test handler)
void OpenBrowser(const QUrl & Url);
// Tests: replaces opening the browser (nullptr restores QDesktopServices::openUrl)
void SetOpenUrlHandler(std::function<void(const QUrl &)> Handler);

// TPasteKeyHandler::Paste: the clipboard text is the host key's SHA-256 or
// MD5 fingerprint (any WinSCP/OpenSSH notation) or its OpenSSH public key line
bool HostKeyMatchesText(const QString & Text, const QString & KeyType, const QString & FingerprintSHA256,
  const QString & FingerprintMD5);

class GuiUserInterface : public QObject, public UserInterface
{
  Q_OBJECT

public:
  explicit GuiUserInterface(QWidget * ParentWidget);
  ~GuiUserInterface() override;

  HostKeyAnswer VerifyHostKey(const QString & Host, int Port, const QString & KeyType,
    const QString & FingerprintSHA256, const QString & FingerprintMD5, HostKeyState State) override;
  bool PromptUser(PromptKind Kind, const QString & Name, const QString & Instructions, QList<Prompt> & Prompts) override;
  void DisplayBanner(const QString & SessionName, const QString & Banner) override;
  unsigned int QueryUser(const QString & Query, const QString & MoreMessages, unsigned int Answers) override;
  unsigned int QueryUserEx(const QString & Query, const QString & MoreMessages, unsigned int Answers,
    const QueryUserParams & Params) override;

  // Authenticate window (TTerminalManager::AuthenticateFormCancel...): call
  // when a session starts opening; shows the connection log, prompts and the
  // banner of that session go into it. OnCancel runs when the user closes the
  // window while it only shows the log (abort the connecting). GUI thread.
  void BeginConnecting(const SessionData & Data, std::function<void()> OnCancel = {});
  // Closes the Authenticate window (connected, failed or aborted). GUI thread.
  void EndConnecting();
  // TTerminal::Information: adds a line to the connection log; any thread
  void Information(const QString & Message);
  AuthenticateDialog * Authenticate() const;

  // Password last entered at a password prompt; background (queue) connections
  // of the same session reuse it, as WinSCP does. Clears it.
  QString TakeLastPassword();
  // "Remember password for this session" was checked at the last password
  // prompt (pupRemember): keep the password for the session although
  // Preferences > Remember password for the session duration is off. Clears it.
  bool TakeSessionRememberPassword();

  // TConfiguration::ShowBanner / NeverShowBanner (Configuration\Banners,
  // "<MD5 of the UTF-16 banner>,<params>"); SessionKey as TSessionData::SessionKey
  static QString BannerHash(const QString & Banner);
  static QString SessionKey(const SessionData & Data);

private:
  QPointer<QWidget> FParentWidget;
  QString FLastPassword;
  bool FSessionRememberPassword = false;
  QPointer<AuthenticateDialog> FAuthenticate;
  std::unique_ptr<SessionData> FConnectData;
  bool FAuthenticatingLogged = false;

  template<typename F> auto InGuiThread(F Func) -> decltype(Func());
  void LogAuthenticating();
  bool IsConnectingSession(const QString & Name) const;
  QWidget * DialogParent() const;
};
