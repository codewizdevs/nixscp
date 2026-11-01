// Port of WinSCP source/forms/Authenticate.{cpp,dfm} (TAuthenticateForm):
// one window for the whole login. The top panel shows the connection log
// (Searching for host..., Connecting to host..., Authenticating...) next to
// the connecting animation; username, password, passphrase and
// keyboard-interactive prompts and the authentication banner are shown in
// the same window, below the log.
#pragma once

#include "core/FileSystem.h"

#include <QDialog>
#include <QList>
#include <QPointer>
#include <QStringList>

#include <functional>

class QCheckBox;
class QEventLoop;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QVBoxLayout;
class QWidget;
class QAction;
class QLineEdit;
class FrameAnimation;

// TTerminal::DisplayBanner options / params (core/Terminal.h)
const int boDisableNeverShowAgain = 0x01;
const unsigned int bpMonospacedFont = 0x01;

class AuthenticateDialog : public QDialog
{
  Q_OBJECT

public:
  explicit AuthenticateDialog(QWidget * Parent);
  ~AuthenticateDialog() override;

  // TAuthenticateForm::Init: SessionName goes to the caption; Secure selects
  // the ConnectingSecure / ConnectingInsecure animation
  void Init(const QString & SessionName, bool Secure);
  // WM_CLOSE / Escape while only the log is shown (TAuthenticateForm::OnCancel)
  void SetOnCancel(std::function<void()> OnCancel) { FOnCancel = std::move(OnCancel); }

  // Shows the window with the log (TAuthenticateForm::ShowAsModal)
  void ShowAsModal();
  // Adds a line to the connection log (TAuthenticateForm::Log)
  void Log(const QString & Message, const QString & Additional = QString());
  QStringList LogLines() const;

  struct PromptOptions
  {
    // in: SavePasswordPanel / SessionRememberPasswordPanel visibility
    bool ShowSavePassword = false;
    bool ShowSessionRememberPassword = false;
    // out: state of the check boxes after OK
    bool SavePassword = false;
    bool SessionRememberPassword = false;
  };
  // TAuthenticateForm::PromptUser. Title is the status shown in the caption
  // ("Password - session"). Fills Prompts[i].Result on OK.
  bool PromptUser(UserInterface::PromptKind Kind, const QString & Title, const QString & Instructions,
    QList<Prompt> & Prompts, PromptOptions & Options);
  // TAuthenticateForm::Banner; Params: bpMonospacedFont
  void Banner(const QString & BannerText, bool & NeverShowAgain, int Options, unsigned int & Params);

  // A prompt or the banner is being shown (nested call would have to wait)
  bool Busy() const { return FLoop != nullptr; }

  // For tests: the panel being shown and its controls
  QWidget * PasswordPanel() const { return FPasswordPanel; }
  QWidget * BannerPanel() const { return FBannerPanel; }
  QListWidget * LogView() const { return FLogView; }
  QList<QLineEdit *> PromptEdits() const { return FEdits; }
  QCheckBox * SavePasswordCheck() const { return FSavePasswordCheck; }
  QCheckBox * SessionRememberPasswordCheck() const { return FSessionRememberPasswordCheck; }
  QCheckBox * NeverShowAgainCheck() const { return FNeverShowAgainCheck; }
  QPlainTextEdit * BannerMemo() const { return FBannerMemo; }
  QAction * BannerMonospacedFontAction() const { return FBannerMonospacedFontAction; }
  QPushButton * PasswordOKButton() const { return FPasswordOKButton; }
  QPushButton * PasswordCancelButton() const { return FPasswordCancelButton; }
  QPushButton * BannerCloseButton() const { return FBannerCloseButton; }
  // Ends the running prompt/banner as with the OK (true) or Cancel button
  void Finish(bool Ok);

  // TAuthenticateForm::ExtractUrl
  static bool ExtractUrl(const QString & Text, QString & Url);

protected:
  void closeEvent(QCloseEvent * Event) override;
  bool eventFilter(QObject * Watched, QEvent * Event) override;
  void reject() override;

private:
  void AdjustControls();
  void GeneratePrompt(UserInterface::PromptKind Kind, const QString & Instructions, const QList<Prompt> & Prompts);
  QLabel * GenerateLabel(const QString & Caption);
  void ExternalLabel(QLabel * Label);
  void UpdateBannerFont();
  bool Execute(const QString & Status, QWidget * Panel, QWidget * FocusControl, QPushButton * DefaultButton,
    bool FixHeight, bool Zoom, bool ForceLog);
  void DoCancel();

  QString FSessionName;
  QString FStatus;
  std::function<void()> FOnCancel;
  QEventLoop * FLoop = nullptr;
  bool FResult = false;
  bool FExecuting = false;

  QWidget * FTopPanel;
  FrameAnimation * FAnimation;
  QListWidget * FLogView;
  QStringList FHints;

  QWidget * FPasswordPanel;
  QWidget * FPromptEditPanel;
  QVBoxLayout * FPromptLayout;
  QList<QLineEdit *> FEdits;
  QWidget * FSavePasswordPanel;
  QCheckBox * FSavePasswordCheck;
  QWidget * FSessionRememberPasswordPanel;
  QCheckBox * FSessionRememberPasswordCheck;
  QPushButton * FPasswordOKButton;
  QPushButton * FPasswordCancelButton;

  QWidget * FBannerPanel;
  QPlainTextEdit * FBannerMemo;
  QCheckBox * FNeverShowAgainCheck;
  QPushButton * FBannerCloseButton;
  QAction * FBannerMonospacedFontAction;
};
