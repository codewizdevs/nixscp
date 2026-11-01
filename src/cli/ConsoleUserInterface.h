// Port of WinSCP source/windows/ConsoleRunner.cpp: TOwnConsole (terminal I/O,
// here for a Linux tty or pipes) and the prompt/query handlers of
// TConsoleRunner (ScriptTerminalPromptUser, ScriptTerminalQueryUser),
// plus the console part of TSecureShell::VerifyHostKey (source/core/SecureShell.cpp).
#pragma once

#include "core/FileSystem.h"
#include "cli/SessionUrl.h"

#include <QMutex>
#include <QRecursiveMutex>
#include <QString>
#include <QStringList>
#include <atomic>
#include <functional>
#include <vector>

class Script;

// TConsole
class Console
{
public:
  static Console & Instance();

  void Print(const QString & Str, bool FromBeginning = false);
  void PrintLine(const QString & Str = QString());
  // TConsoleRunner::ScriptPrintProgress: overwrites the previous progress line
  void PrintProgress(bool First, const QString & Str);
  // Ends a pending progress line
  void FinishProgress();

  // Reads a line; returns false on EOF, interrupt or timeout (TimeoutMs > 0)
  bool Input(QString & Str, bool Echo, unsigned int TimeoutMs = 0);
  // Interactive command prompt ("winscp> "): on a terminal with line editing
  // (Left/Right/Home/End/Backspace/Delete, Ctrl+A/E/B/F/K/U/W/L) and command
  // history (Up/Down, Ctrl+P/N); otherwise a plain line read.
  bool ReadCommandLine(const QString & Prompt, QString & Str);
  // Reads a single choice; returns 1-based index into Accels, Cancel on Esc,
  // Break on Ctrl+C/EOF, Timeouted on timeout (TimeoutMs > 0)
  int Choice(const QString & Accels, int Cancel, int Break, int Timeouted, unsigned int TimeoutMs);

  // Whether the status output (stdout, or stderr with /stdout) is a terminal
  bool OutputIsTty() const { return FUseStdErr ? FStdErrTty : FOutputTty; }
  bool InputIsTty() const { return FInputTty; }

  // /stdout: status output goes to stderr, stdout carries file data only
  void SetUseStdErr(bool Value);
  bool UseStdErr() const { return FUseStdErr; }
  // Writes raw (binary) data to stdout (get ... - with /stdout)
  void WriteStdOut(const QByteArray & Data);
  // /nointeractiveinput (or /stdin): every prompt is cancelled
  void SetNoInteractiveInput(bool Value) { FNoInteractiveInput = Value; }
  bool NoInteractiveInput() const { return FNoInteractiveInput; }
  // TConsoleRunner::UpdateTitle: terminal window title (OSC 0) when the
  // status output is a terminal
  void SetTitle(const QString & Title);
  // Restores the title the terminal had before the first SetTitle (xterm title stack)
  void RestoreTitle();

  // Ctrl+C handling (TOwnConsole::PendingAbort)
  void InstallSignalHandler();
  bool PendingAbort() const;
  void ClearPendingAbort();

  QRecursiveMutex & Mutex() { return FMutex; }

private:
  Console();
  void Write(const QString & Str);

  QRecursiveMutex FMutex;
  bool FOutputTty = false;
  bool FInputTty = false;
  bool FStdErrTty = false;
  bool FUseStdErr = false;
  bool FNoInteractiveInput = false;
  int FLastProgressLen = 0;
  QStringList FHistory;
  QString FTitle;
  bool FTitlePushed = false;
};

// Per session UserInterface (TTerminal::OnPromptUser/OnQueryUser/OnInformation
// handlers of TManagementScript and TConsoleRunner). Called on the Terminal's
// worker thread while the main thread waits for the operation.
class ConsoleUserInterface : public UserInterface
{
public:
  ConsoleUserInterface(Script * AScript, const SessionData & Data, const SessionExtras & Extras);

  HostKeyAnswer VerifyHostKey(const QString & Host, int Port, const QString & KeyType,
    const QString & FingerprintSHA256, const QString & FingerprintMD5, HostKeyState State) override;
  bool PromptUser(PromptKind Kind, const QString & Name, const QString & Instructions, QList<Prompt> & Prompts) override;
  // An authentication prompt was cancelled (stored credentials exhausted,
  // batch mode, end of input): TSecureShell::FAuthenticationCancelled
  bool AuthenticationCancelled() const { return FAuthenticationCancelled; }
  // Credentials were given for the session (TSecureShell::GetStoredCredentialsTried)
  bool StoredCredentialsTried() const;
  void DisplayBanner(const QString & SessionName, const QString & Banner) override;
  unsigned int QueryUser(const QString & Query, const QString & MoreMessages, unsigned int Answers) override;
  // Button aliases (captions), countdown, qpWaitInBatch, NoBatchAnswers
  unsigned int QueryUserEx(const QString & Query, const QString & MoreMessages, unsigned int Answers,
    const QueryUserParams & Params) override;

  // Connection phase messages ("Authenticating..."); main thread only between operations
  void SetOpening(bool Opening);
  void PrintAuthenticating();
  // Replacement error for a failed host key verification (CONFIGURED_KEY_NOT_MATCH)
  // and the "Host key fingerprint is ..." detail line
  QString HostKeyError() const { return FHostKeyError; }
  QString HostKeyErrorDetail() const { return FHostKeyErrorDetail; }

  struct QueryButton
  {
    unsigned int Answer;
    QString Caption;                         // empty = default caption of Answer
    std::function<bool()> OnSubmit;          // returns false to ask again
  };
  struct QueryParams
  {
    bool WaitInBatch = false;                // qpWaitInBatch
    unsigned int NoBatchAnswers = 0;
    unsigned int Timeout = 0;                // TQueryParams::Timeout (ms, timer-driven answer)
    unsigned int TimeoutAnswer = 0;
    bool FatalAbort = false;                 // qpFatalAbort: Abort closes the session
  };
  // ScriptTerminalQueryUser
  unsigned int Query(const QString & Query, const QString & MoreMessages, const std::vector<QueryButton> & Buttons,
    const QueryParams & Params);

private:
  bool DoPromptUser(PromptKind Kind, const QString & Name, const QString & Instructions, QList<Prompt> & Prompts);

  Script * FScript;
  SessionData FData;
  SessionExtras FExtras;
  bool FPassphraseUsed = false;
  std::atomic<bool> FAuthenticationCancelled{false};
  bool FEmptyPasswordUsed = false;
  std::atomic<bool> FOpening{false};
  std::atomic<bool> FAuthenticatingPrinted{false};
  QString FHostKeyError;
  QString FHostKeyErrorDetail;

  bool DoInput(QString & Str, bool Echo, unsigned int Timeout);
};

// CancelAnswer/AbortAnswer/ContinueAnswer (source/core/Common.cpp)
unsigned int CancelAnswer(unsigned int Answers);
unsigned int AbortAnswer(unsigned int Answers);
unsigned int ContinueAnswer(unsigned int Answers);
