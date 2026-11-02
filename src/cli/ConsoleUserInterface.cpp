// Port of WinSCP source/windows/ConsoleRunner.cpp (TOwnConsole, TConsoleRunner
// prompt/query handlers) and the host key prompt of source/core/SecureShell.cpp
#include "cli/ConsoleUserInterface.h"
#include "cli/Script.h"

#include <QElapsedTimer>
#include <QMutexLocker>

#include <signal.h>
#include <unistd.h>
#ifndef Q_OS_WIN
#include <poll.h>
#include <termios.h>
#endif
#include <algorithm>
#include <cerrno>
#include <cstdio>

static std::atomic<int> InterruptCount{0};

static void InterruptHandler(int)
{
  // Second Ctrl+C while the first one was not handled yet terminates hard
  if (InterruptCount.fetch_add(1) >= 1)
  {
    _exit(1);
  }
}

// Text of the console: UTF-8 on Windows (SetUpWindowsConsole), the locale's encoding elsewhere
static QByteArray ToConsole(const QString & Str)
{
#ifdef Q_OS_WIN
  return Str.toUtf8();
#else
  return Str.toLocal8Bit();
#endif
}

static QString FromConsole(const QByteArray & Data)
{
#ifdef Q_OS_WIN
  return QString::fromUtf8(Data);
#else
  return QString::fromLocal8Bit(Data);
#endif
}

Console & Console::Instance()
{
  static Console Instance;
  return Instance;
}

#ifdef Q_OS_WIN
static BOOL WINAPI ConsoleCtrlHandler(DWORD Event)
{
  if ((Event == CTRL_C_EVENT) || (Event == CTRL_BREAK_EVENT))
  {
    InterruptHandler(SIGINT);
    return TRUE;
  }
  return FALSE;
}

// UTF-8 in and out, and the escape sequences (window title) of Windows 10+
static void SetUpWindowsConsole()
{
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
  for (DWORD Std : {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE})
  {
    HANDLE Handle = GetStdHandle(Std);
    DWORD Mode = 0;
    if ((Handle != INVALID_HANDLE_VALUE) && GetConsoleMode(Handle, &Mode))
    {
      SetConsoleMode(Handle, Mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
  }
}
#endif

Console::Console()
{
#ifdef Q_OS_WIN
  SetUpWindowsConsole();
#endif
  FOutputTty = (isatty(STDOUT_FILENO) != 0);
  FInputTty = (isatty(STDIN_FILENO) != 0);
  FStdErrTty = (isatty(STDERR_FILENO) != 0);
}

void Console::SetUseStdErr(bool Value)
{
  QMutexLocker Lock(&FMutex);
  FUseStdErr = Value;
}

void Console::WriteStdOut(const QByteArray & Data)
{
  QMutexLocker Lock(&FMutex);
  fwrite(Data.constData(), 1, static_cast<size_t>(Data.size()), stdout);
  fflush(stdout);
}

void Console::SetTitle(const QString & Title)
{
  QMutexLocker Lock(&FMutex);
  if (OutputIsTty() && (Title != FTitle))
  {
    FTitle = Title;
    // OSC 0: icon name and window title
    QString Sanitized = Title;
    Sanitized.remove(QLatin1Char('\x07'));
    Sanitized.remove(QLatin1Char('\x1b'));
    QString Out;
    if (!FTitlePushed)
    {
      // save the current title on the terminal's title stack (XTWINOPS 22)
      Out = QStringLiteral("\x1b[22;0t");
      FTitlePushed = true;
    }
    Write(Out + QStringLiteral("\x1b]0;%1\x07").arg(Sanitized));
  }
}

void Console::RestoreTitle()
{
  QMutexLocker Lock(&FMutex);
  if (FTitlePushed)
  {
    Write(QStringLiteral("\x1b[23;0t"));
    FTitlePushed = false;
    FTitle.clear();
  }
}

void Console::InstallSignalHandler()
{
#ifdef Q_OS_WIN
  SetConsoleCtrlHandler(&ConsoleCtrlHandler, TRUE);
#else
  struct sigaction Action = {};
  Action.sa_handler = InterruptHandler;
  sigemptyset(&Action.sa_mask);
  Action.sa_flags = 0;
  sigaction(SIGINT, &Action, nullptr);
#endif
}

bool Console::PendingAbort() const
{
  return (InterruptCount.load() > 0);
}

void Console::ClearPendingAbort()
{
  InterruptCount = 0;
}

void Console::Write(const QString & Str)
{
  QByteArray Bytes = ToConsole(Str);
  FILE * Stream = FUseStdErr ? stderr : stdout;
  fwrite(Bytes.constData(), 1, static_cast<size_t>(Bytes.size()), Stream);
  fflush(Stream);
}

void Console::Print(const QString & Str, bool FromBeginning)
{
  QMutexLocker Lock(&FMutex);
  QString S = Str;
  if (FLastProgressLen > 0)
  {
    S = QLatin1Char('\n') + S;
    FLastProgressLen = 0;
  }
  else if (FromBeginning && OutputIsTty())
  {
    S = QLatin1Char('\r') + S;
  }
  Write(S);
}

void Console::PrintLine(const QString & Str)
{
  Print(Str + QLatin1Char('\n'));
}

void Console::PrintProgress(bool First, const QString & Str)
{
  QMutexLocker Lock(&FMutex);
  QString S = Str;
  if (First && (FLastProgressLen > 0))
  {
    S = QLatin1Char('\n') + S;
  }
  else if (S.size() < FLastProgressLen)
  {
    int Padding = FLastProgressLen - static_cast<int>(S.size());
    S += QString(Padding, QLatin1Char(' ')) + QString(Padding, QLatin1Char('\b'));
  }
  Write(QLatin1Char('\r') + S);
  FLastProgressLen = static_cast<int>(Str.size());
}

void Console::FinishProgress()
{
  QMutexLocker Lock(&FMutex);
  if (FLastProgressLen > 0)
  {
    Write(QStringLiteral("\n"));
    FLastProgressLen = 0;
  }
}

namespace
{
// Switches the terminal to no-echo (and optionally non-canonical) mode for its lifetime
#ifdef Q_OS_WIN
class TerminalMode
{
public:
  TerminalMode(bool Active, bool Echo, bool Canonical) :
    FInput(GetStdHandle(STD_INPUT_HANDLE)), FActive(Active && GetConsoleMode(FInput, &FPrev))
  {
    if (FActive)
    {
      DWORD Mode = FPrev;
      if (!Echo)
      {
        Mode &= ~ENABLE_ECHO_INPUT;
      }
      if (!Canonical)
      {
        Mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
      }
      SetConsoleMode(FInput, Mode);
    }
  }
  ~TerminalMode()
  {
    if (FActive)
    {
      SetConsoleMode(FInput, FPrev);
    }
  }
private:
  HANDLE FInput;
  DWORD FPrev = 0;
  bool FActive;
};

// Whether a byte can be read from stdin without blocking for long
static bool InputAvailable(HANDLE Input, DWORD TimeoutMs)
{
  DWORD Mode = 0;
  if (GetConsoleMode(Input, &Mode))
  {
    // Console: key presses (other events, as focus or mouse, are dropped)
    if (WaitForSingleObject(Input, TimeoutMs) != WAIT_OBJECT_0)
    {
      return false;
    }
    INPUT_RECORD Record;
    DWORD Count = 0;
    while (PeekConsoleInputW(Input, &Record, 1, &Count) && (Count == 1))
    {
      if ((Record.EventType == KEY_EVENT) && Record.Event.KeyEvent.bKeyDown)
      {
        return true;
      }
      ReadConsoleInputW(Input, &Record, 1, &Count);
    }
    return false;
  }
  if (GetFileType(Input) == FILE_TYPE_PIPE)
  {
    DWORD Available = 0;
    if (!PeekNamedPipe(Input, nullptr, 0, nullptr, &Available, nullptr))
    {
      return true;   // closed: ReadFile reports EOF
    }
    if (Available == 0)
    {
      Sleep(TimeoutMs);
      return false;
    }
    return true;
  }
  return true;   // a file: never blocks
}
#else
class TerminalMode
{
public:
  TerminalMode(bool Active, bool Echo, bool Canonical) : FActive(Active && (tcgetattr(STDIN_FILENO, &FPrev) == 0))
  {
    if (FActive)
    {
      struct termios Mode = FPrev;
      if (!Echo)
      {
        Mode.c_lflag &= ~static_cast<tcflag_t>(ECHO);
      }
      if (!Canonical)
      {
        Mode.c_lflag &= ~static_cast<tcflag_t>(ICANON);
        Mode.c_cc[VMIN] = 1;
        Mode.c_cc[VTIME] = 0;
      }
      tcsetattr(STDIN_FILENO, TCSANOW, &Mode);
    }
  }
  ~TerminalMode()
  {
    if (FActive)
    {
      tcsetattr(STDIN_FILENO, TCSANOW, &FPrev);
    }
  }
private:
  struct termios FPrev = {};
  bool FActive;
};
#endif

// Waits for one byte on stdin; 1 = read, 0 = EOF/error, -1 = timeout, -2 = interrupted
int ReadByte(char & C, QElapsedTimer & Timer, unsigned int TimeoutMs)
{
  for (;;)
  {
    if (Console::Instance().PendingAbort())
    {
      return -2;
    }
    if ((TimeoutMs > 0) && (Timer.elapsed() >= static_cast<qint64>(TimeoutMs)))
    {
      return -1;
    }
#ifdef Q_OS_WIN
    HANDLE Input = GetStdHandle(STD_INPUT_HANDLE);
    if (!InputAvailable(Input, 100))
    {
      continue;
    }
    DWORD Read = 0;
    if (!ReadFile(Input, &C, 1, &Read, nullptr))
    {
      // Ctrl+C aborts a pending console read
      if ((GetLastError() == ERROR_OPERATION_ABORTED) && !Console::Instance().PendingAbort())
      {
        continue;
      }
      return Console::Instance().PendingAbort() ? -2 : 0;
    }
    if ((Read == 1) && (C == '\r'))
    {
      continue;   // CR LF line ends
    }
    return (Read == 1) ? 1 : 0;
#else
    struct pollfd Fd = {STDIN_FILENO, POLLIN, 0};
    int Rc = poll(&Fd, 1, 100);
    if (Rc < 0)
    {
      if (errno == EINTR)
      {
        continue;
      }
      return 0;
    }
    if (Rc == 0)
    {
      continue;
    }
    ssize_t Read = read(STDIN_FILENO, &C, 1);
    if (Read < 0)
    {
      if ((errno == EINTR) || (errno == EAGAIN))
      {
        continue;
      }
      return 0;
    }
    return (Read == 1) ? 1 : 0;
#endif
  }
}
}

bool Console::Input(QString & Str, bool Echo, unsigned int TimeoutMs)
{
  // Input is not done under the output mutex, so the main thread may print meanwhile
  if (FNoInteractiveInput)
  {
    Str.clear();
    return false;
  }
  bool Result = false;
  bool Eof = false;
  QByteArray Line;
  {
    TerminalMode Mode(FInputTty && !Echo, Echo, true);
    QElapsedTimer Timer;
    Timer.start();
    for (;;)
    {
      char C = 0;
      int Rc = ReadByte(C, Timer, TimeoutMs);
      if (Rc == 1)
      {
        if (C == '\n')
        {
          Result = true;
          break;
        }
        Line += C;
        // any key press restarts the timeout (TOwnConsole::Input)
        Timer.restart();
      }
      else
      {
        // the last line without a newline is still a line
        Result = (Rc == 0) && !Line.isEmpty();
        Eof = (Rc == 0) && Line.isEmpty();
        break;
      }
    }
  }
  if ((FInputTty && !Echo) || Eof)
  {
    // also on EOF, so that the following message does not continue the prompt line
    Print(QStringLiteral("\n"));
  }
  if (Line.endsWith('\r'))
  {
    Line.chop(1);
  }
  Str = FromConsole(Line);
  return Result;
}

namespace
{
// Display width of a string on the terminal (wide CJK characters count 2)
int DisplayWidth(const QString & Str)
{
  int Result = 0;
  for (qsizetype I = 0; I < Str.size(); I++)
  {
    char32_t C = Str[I].unicode();
    if (Str[I].isHighSurrogate() && (I + 1 < Str.size()))
    {
      C = QChar::surrogateToUcs4(Str[I], Str[I + 1]);
      I++;
    }
    bool Wide = ((C >= 0x1100) && (C <= 0x115F)) || ((C >= 0x2E80) && (C <= 0xA4CF)) || ((C >= 0xAC00) && (C <= 0xD7A3)) ||
      ((C >= 0xF900) && (C <= 0xFAFF)) || ((C >= 0xFE30) && (C <= 0xFE4F)) || ((C >= 0xFF00) && (C <= 0xFF60)) ||
      ((C >= 0xFFE0) && (C <= 0xFFE6)) || ((C >= 0x1F300) && (C <= 0x1FAFF)) || ((C >= 0x20000) && (C <= 0x3FFFD));
    Result += Wide ? 2 : 1;
  }
  return Result;
}
}

bool Console::ReadCommandLine(const QString & Prompt, QString & Str)
{
  if (FNoInteractiveInput)
  {
    Print(Prompt);
    Str.clear();
    return false;
  }
  if (!FInputTty || !OutputIsTty())
  {
    Print(Prompt);
    return Input(Str, true, 0);
  }

  // Minimal line editor (the role of the Windows console's own line editing
  // and F7 history in winscp.com)
  Print(Prompt);
  QString Line;
  int Pos = 0;                       // cursor, index into Line
  int HistoryIndex = static_cast<int>(FHistory.size());
  QString Pending;                   // the line being edited while browsing history
  bool Result = false;
  QElapsedTimer Timer;
  Timer.start();
  auto Redraw = [&]()
  {
    // Go to the start of the input, rewrite it and clear to the end of line
    QString Out = QStringLiteral("\r") + Prompt + Line + QStringLiteral("\x1b[K");
    int Back = DisplayWidth(Line.mid(Pos));
    if (Back > 0)
    {
      Out += QStringLiteral("\x1b[%1D").arg(Back);
    }
    QMutexLocker Lock(&FMutex);
    Write(Out);
  };
  auto WordStart = [&](int From)
  {
    int P = From;
    while ((P > 0) && Line[P - 1].isSpace()) P--;
    while ((P > 0) && !Line[P - 1].isSpace()) P--;
    return P;
  };
  auto WordEnd = [&](int From)
  {
    int P = From;
    int Len = static_cast<int>(Line.size());
    while ((P < Len) && Line[P].isSpace()) P++;
    while ((P < Len) && !Line[P].isSpace()) P++;
    return P;
  };
  auto BrowseHistory = [&](bool Up)
  {
    if (Up)
    {
      if (HistoryIndex > 0)
      {
        if (HistoryIndex == FHistory.size()) { Pending = Line; }
        HistoryIndex--;
        Line = FHistory[HistoryIndex];
        Pos = static_cast<int>(Line.size());
        Redraw();
      }
    }
    else if (HistoryIndex < FHistory.size())
    {
      HistoryIndex++;
      Line = (HistoryIndex == FHistory.size()) ? Pending : FHistory[HistoryIndex];
      Pos = static_cast<int>(Line.size());
      Redraw();
    }
  };
  {
    TerminalMode Mode(true, false, false);
    QByteArray Utf8Pending;
    for (;;)
    {
      char C = 0;
      int Rc = ReadByte(C, Timer, 0);
      if (Rc != 1)
      {
        // Ctrl+C or end of input
        Result = (Rc == 0) && !Line.isEmpty();
        break;
      }
      unsigned char U = static_cast<unsigned char>(C);
      if ((U == '\n') || (U == '\r'))
      {
        Result = true;
        break;
      }
      else if (U == 4)               // Ctrl+D: EOF on an empty line, else delete
      {
        if (Line.isEmpty())
        {
          Result = false;
          break;
        }
        if (Pos < Line.size()) { Line.remove(Pos, 1); Redraw(); }
      }
      else if ((U == 127) || (U == 8))   // Backspace
      {
        if (Pos > 0) { Line.remove(Pos - 1, 1); Pos--; Redraw(); }
      }
      else if (U == 1) { Pos = 0; Redraw(); }                                       // Ctrl+A
      else if (U == 5) { Pos = static_cast<int>(Line.size()); Redraw(); }           // Ctrl+E
      else if (U == 2) { if (Pos > 0) { Pos--; Redraw(); } }                        // Ctrl+B
      else if (U == 6) { if (Pos < Line.size()) { Pos++; Redraw(); } }              // Ctrl+F
      else if (U == 11) { Line.truncate(Pos); Redraw(); }                           // Ctrl+K
      else if (U == 21) { Line.remove(0, Pos); Pos = 0; Redraw(); }                 // Ctrl+U
      else if (U == 23)                                                             // Ctrl+W
      {
        int P = WordStart(Pos);
        Line.remove(P, Pos - P);
        Pos = P;
        Redraw();
      }
      else if (U == 12)                                                             // Ctrl+L
      {
        QMutexLocker Lock(&FMutex);
        Write(QStringLiteral("\x1b[H\x1b[2J"));
        Redraw();
      }
      else if ((U == 16) || (U == 14))                                              // Ctrl+P / Ctrl+N
      {
        BrowseHistory(U == 16);
      }
      else if (U == 27)              // escape sequence
      {
        char S1 = 0;
        if (ReadByte(S1, Timer, 0) != 1) { continue; }
        if ((S1 == 'b') || (S1 == 'f'))   // Alt+B / Alt+F
        {
          Pos = (S1 == 'b') ? WordStart(Pos) : WordEnd(Pos);
          Redraw();
          continue;
        }
        if ((S1 != '[') && (S1 != 'O')) { continue; }
        QByteArray Seq;
        char S2 = 0;
        while (ReadByte(S2, Timer, 0) == 1)
        {
          Seq += S2;
          if (((S2 >= 'A') && (S2 <= 'Z')) || ((S2 >= 'a') && (S2 <= 'z')) || (S2 == '~'))
          {
            break;
          }
        }
        if (Seq.isEmpty()) { continue; }
        char Final = Seq.back();
        if ((Final == 'A') || (Final == 'B'))
        {
          BrowseHistory(Final == 'A');
        }
        else if (Final == 'C')
        {
          if (Seq.contains(";5") || Seq.contains(";3")) { Pos = WordEnd(Pos); Redraw(); }
          else if (Pos < Line.size()) { Pos++; Redraw(); }
        }
        else if (Final == 'D')
        {
          if (Seq.contains(";5") || Seq.contains(";3")) { Pos = WordStart(Pos); Redraw(); }
          else if (Pos > 0) { Pos--; Redraw(); }
        }
        else if ((Final == 'H') || (Seq == "1~") || (Seq == "7~")) { Pos = 0; Redraw(); }
        else if ((Final == 'F') || (Seq == "4~") || (Seq == "8~")) { Pos = static_cast<int>(Line.size()); Redraw(); }
        else if (Seq == "3~") { if (Pos < Line.size()) { Line.remove(Pos, 1); Redraw(); } }
      }
      else if (U >= 32)
      {
        // UTF-8 multibyte sequences are collected before inserting
        Utf8Pending += C;
        int Need = 1;
        unsigned char Lead = static_cast<unsigned char>(Utf8Pending[0]);
        if (Lead >= 0xF0) Need = 4;
        else if (Lead >= 0xE0) Need = 3;
        else if (Lead >= 0xC0) Need = 2;
        if (Utf8Pending.size() >= Need)
        {
          QString Chars = QString::fromUtf8(Utf8Pending);
          Utf8Pending.clear();
          bool AtEnd = (Pos == Line.size());
          Line.insert(Pos, Chars);
          Pos += static_cast<int>(Chars.size());
          if (AtEnd)
          {
            // typing at the end: just echo
            QMutexLocker Lock(&FMutex);
            Write(Chars);
          }
          else
          {
            Redraw();
          }
        }
      }
    }
  }
  {
    QMutexLocker Lock(&FMutex);
    Write(QStringLiteral("\n"));
  }
  Str = Line;
  if (Result && !Line.trimmed().isEmpty() && (FHistory.isEmpty() || (FHistory.last() != Line)))
  {
    FHistory.append(Line);
  }
  return Result;
}

int Console::Choice(const QString & Accels, int Cancel, int Break, int Timeouted, unsigned int TimeoutMs)
{
  if (FNoInteractiveInput)
  {
    // USAGE_INTERACTIVEINPUT: all prompts are automatically cancelled
    return Cancel;
  }
  QElapsedTimer Timer;
  Timer.start();
  if (FInputTty)
  {
    TerminalMode Mode(true, false, false);
    for (;;)
    {
      char C = 0;
      int Rc = ReadByte(C, Timer, TimeoutMs);
      if (Rc == -1)
      {
        return Timeouted;
      }
      if (Rc != 1)
      {
        return Break;
      }
      if (C == 27)
      {
        return Cancel;
      }
      int Index = static_cast<int>(Accels.indexOf(QChar(QLatin1Char(C)).toUpper()));
      if (Index >= 0)
      {
        return Index + 1;
      }
    }
  }
  else
  {
    // Pipe: one answer per line, first character decides
    for (;;)
    {
      QByteArray Line;
      int Rc = 1;
      for (;;)
      {
        char C = 0;
        Rc = ReadByte(C, Timer, TimeoutMs);
        if ((Rc != 1) || (C == '\n'))
        {
          break;
        }
        Line += C;
      }
      QString Answer = FromConsole(Line).trimmed();
      if (!Answer.isEmpty())
      {
        int Index = static_cast<int>(Accels.indexOf(Answer[0].toUpper()));
        if (Index >= 0)
        {
          return Index + 1;
        }
      }
      if (Rc == -1)
      {
        return Timeouted;
      }
      if (Rc != 1)
      {
        return Break;
      }
    }
  }
}

unsigned int CancelAnswer(unsigned int Answers)
{
  if (Answers & qaCancel) return qaCancel;
  if (Answers & qaNo) return qaNo;
  if (Answers & qaAbort) return qaAbort;
  if (Answers & qaOK) return qaOK;
  return qaCancel;
}

unsigned int AbortAnswer(unsigned int Answers)
{
  return (Answers & qaAbort) ? static_cast<unsigned int>(qaAbort) : CancelAnswer(Answers);
}

unsigned int ContinueAnswer(unsigned int Answers)
{
  if (Answers & qaSkip) return qaSkip;
  if (Answers & qaYes) return qaYes;
  if (Answers & qaOK) return qaOK;
  if (Answers & qaRetry) return qaRetry;
  return CancelAnswer(Answers);
}

// AnswerNameAndCaption (source/forms/MessageDlg.cpp), in WinSCP's button order
static const struct { unsigned int Answer; const char * Caption; } AnswerCaptions[] =
{
  { qaYes, "&Yes" },
  { qaNo, "&No" },
  { qaOK, "OK" },
  { qaCancel, "Cancel" },
  { qaAbort, "&Abort" },
  { qaRetry, "&Retry" },
  { qaIgnore, "&Ignore" },
  { qaAll, "A&ll" },
  { qaNoToAll, "N&o to All" },
  { qaYesToAll, "Yes to A&ll" },
  { qaSkip, "&Skip" },
  { qaNeverAskAgain, "Never ask me a&gain" },
};

ConsoleUserInterface::ConsoleUserInterface(Script * AScript, const SessionData & Data, const SessionExtras & Extras) :
  FScript(AScript),
  FData(Data),
  FExtras(Extras)
{
}

void ConsoleUserInterface::SetOpening(bool Opening)
{
  FOpening = Opening;
}

void ConsoleUserInterface::PrintAuthenticating()
{
  if (FOpening && !FAuthenticatingPrinted.exchange(true))
  {
    FScript->PrintLine(QStringLiteral("Authenticating..."));
    if (!FData.UserName.isEmpty())
    {
      FScript->PrintLine(QStringLiteral("Using username \"%1\".").arg(FData.UserName));
    }
  }
}

bool ConsoleUserInterface::DoInput(QString & Str, bool Echo, unsigned int Timeout)
{
  bool Result;
  // NixSCP: in batch mode never consume a non-interactive stdin (it may be the script)
  if ((FScript->Batch() != Script::BatchOff) && !Console::Instance().InputIsTty())
  {
    Result = false;
  }
  else
  {
    Result = Console::Instance().Input(Str, Echo, Timeout);
  }
  if (!Result)
  {
    FScript->NotifyAbort();
  }
  return Result;
}

UserInterface::HostKeyAnswer ConsoleUserInterface::VerifyHostKey(const QString & Host, int Port, const QString & KeyType,
  const QString & FingerprintSHA256, const QString & FingerprintMD5, HostKeyState State)
{
  PrintAuthenticating();

  // TSecureShell::VerifyHostKey: "<type> <bits> <hash>" format of fingerprints
  QString Bits;
  if (KeyType == QStringLiteral("ssh-ed25519")) Bits = QStringLiteral("255");
  else if (KeyType == QStringLiteral("ssh-ed448")) Bits = QStringLiteral("448");
  else if (KeyType.endsWith(QStringLiteral("nistp256"))) Bits = QStringLiteral("256");
  else if (KeyType.endsWith(QStringLiteral("nistp384"))) Bits = QStringLiteral("384");
  else if (KeyType.endsWith(QStringLiteral("nistp521"))) Bits = QStringLiteral("521");
  QString SignKeyType = Bits.isEmpty() ? KeyType : (KeyType + QLatin1Char(' ') + Bits);
  QString SHA256 = FingerprintSHA256;
  if (SHA256.startsWith(QStringLiteral("SHA256:")))
  {
    SHA256 = SHA256.mid(7);
  }
  QString MD5 = FingerprintMD5;
  if (MD5.startsWith(QStringLiteral("MD5:")))
  {
    MD5 = MD5.mid(4);
  }
  QString FullSHA256 = SignKeyType + QLatin1Char(' ') + SHA256;
  QString FullMD5 = SignKeyType + QLatin1Char(' ') + MD5;
  FScript->Log(QLatin1Char('.'), QStringLiteral("Verifying host key %1 with fingerprints %2, %3").arg(KeyType, FullSHA256, FullMD5));

  // A configured fingerprint (-hostkey, ;fingerprint=, SshHostKey) is
  // verified by the core (VerifySessionHostKey) before the cache is asked,
  // so this is only reached for an unknown or changed key without one.
  FHostKeyError.clear();
  FHostKeyErrorDetail.clear();

  QString Message;
  QString Question;
  if (State == HostKeyUnknown)
  {
    Message =
      QStringLiteral("Continue connecting to an unknown server and add its host key to a cache?\n\n"
        "The server's host key was not found in the cache. You have no guarantee that the server is the computer you think it is.\n\n"
        "%1 (port %2)\n\n"
        "The server's %3 key fingerprint is:\n%4\n\n"
        "If you trust this host, press Yes. To connect without adding host key to the cache, press No. "
        "To abandon the connection press Abort.").arg(Host).arg(Port).arg(KeyType, FullSHA256);
    Question = QStringLiteral("Continue connecting and add host key to the cache?");
  }
  else
  {
    Message =
      QStringLiteral("WARNING - POTENTIAL SECURITY BREACH!\n\n"
        "The server's host key does not match the one NixSCP has in cache. This means that either the server administrator "
        "has changed the host key, or you have actually connected to another computer pretending to be the server.\n\n"
        "%1 (port %2)\n\n"
        "The new %3 key fingerprint is:\n%4\n\n"
        "If you were expecting this change, trust the new key and want to continue connecting to the server, press Yes "
        "to update the cache. If you want to carry on connecting but without updating the cache, press No. "
        "If you want to abandon the connection completely, press Abort. Pressing Abort is the ONLY guaranteed safe choice.")
        .arg(Host).arg(Port).arg(KeyType, FullSHA256);
    Question = QStringLiteral("Update cached key and continue connecting?");
  }
  // SCRIPTING_USE_HOSTKEY
  Message += QStringLiteral("\n\nIn scripting, you should use a -hostkey switch to configure the expected host key.\n\n") + Question;

  std::vector<QueryButton> Buttons;
  Buttons.push_back(QueryButton{qaYes, QString(), nullptr});
  Buttons.push_back(QueryButton{qaNo, QString(), nullptr});
  Buttons.push_back(QueryButton{qaRetry, QStringLiteral("&Copy key"), [this, FullSHA256, FullMD5]()
    {
      // No clipboard on a console: print the fingerprints to copy them from
      FScript->PrintLine(FullSHA256);
      FScript->PrintLine(FullMD5);
      return false;
    }});
  Buttons.push_back(QueryButton{qaAbort, QString(), nullptr});
  QueryParams Params;
  Params.WaitInBatch = true;
  Params.NoBatchAnswers = qaYes | qaNo | qaRetry;
  unsigned int Answer = Query(Message, QString(), Buttons, Params);
  switch (Answer)
  {
    case qaYes:
      return HostKeyAccept;
    case qaNo:
      return HostKeyOnce;
    default:
      // KEY_NOT_VERIFIED + HOSTKEY
      FHostKeyError = QStringLiteral("Host key wasn't verified!");
      FHostKeyErrorDetail = QStringLiteral("Host key fingerprint is %1.").arg(FullSHA256);
      return HostKeyCancel;
  }
}

bool ConsoleUserInterface::StoredCredentialsTried() const
{
  return !FData.Password.isEmpty() || FExtras.EmptyPassword || !FExtras.Passphrase.isEmpty();
}

bool ConsoleUserInterface::PromptUser(PromptKind Kind, const QString & Name, const QString & Instructions, QList<Prompt> & Prompts)
{
  bool Result = DoPromptUser(Kind, Name, Instructions, Prompts);
  if (!Result && (Kind != pkFileName))
  {
    FAuthenticationCancelled = true;
  }
  return Result;
}

bool ConsoleUserInterface::DoPromptUser(PromptKind Kind, const QString & /*Name*/, const QString & Instructions, QList<Prompt> & Prompts)
{
  if ((Kind == pkPassphrase) && !FExtras.Passphrase.isEmpty())
  {
    if (FPassphraseUsed || (Prompts.size() != 1))
    {
      return false;
    }
    Prompts[0].Result = FExtras.Passphrase;
    FPassphraseUsed = true;
    return true;
  }
  // DenormalizeString: an explicitly empty password ("user:@host",
  // -password=) answers the first password prompt
  // (FTP, WebDAV and S3 log in with the session's password without prompting)
  bool UsesSsh = (FData.FSProtocol != fsFTP) && (FData.FSProtocol != fsWebDAV) && (FData.FSProtocol != fsS3);
  if ((Kind == pkPassword) && FExtras.EmptyPassword && FData.Password.isEmpty() && (Prompts.size() == 1) && UsesSsh)
  {
    if (FEmptyPasswordUsed)
    {
      return false;
    }
    Prompts[0].Result.clear();
    FEmptyPasswordUsed = true;
    return true;
  }
  // -newpassword: answers the server's request to change an expired password
  // (TSecureShell's password change prompts: "New password" / "Confirm new password")
  if (!FExtras.NewPassword.isEmpty() && ((Kind == pkKeybInteractive) || (Kind == pkPassword)) && !Prompts.isEmpty())
  {
    // PAM asks "Current password" (answered with the session password, as
    // TSecureShell answers the change request with the old one), then
    // "New password" and "Retype new password"
    auto IsNew = [](const Prompt & P)
    {
      return P.Text.contains(QStringLiteral("new password"), Qt::CaseInsensitive) ||
        P.Text.contains(QStringLiteral("retype"), Qt::CaseInsensitive);
    };
    auto IsCurrent = [this](const Prompt & P)
    {
      return (P.Text.contains(QStringLiteral("current"), Qt::CaseInsensitive) ||
        P.Text.contains(QStringLiteral("old password"), Qt::CaseInsensitive)) && !FData.Password.isEmpty();
    };
    bool AllKnown = true;
    bool AnyNew = false;
    for (const Prompt & P : Prompts)
    {
      if (IsNew(P))
      {
        AnyNew = true;
      }
      else if (!IsCurrent(P))
      {
        AllKnown = false;
      }
    }
    if (AllKnown && (AnyNew || (Kind == pkKeybInteractive)))
    {
      for (Prompt & P : Prompts)
      {
        P.Result = IsNew(P) ? FExtras.NewPassword : FData.Password;
      }
      return true;
    }
  }
  // TManagementScript::TerminalPromptUser: when authentication using stored
  // password fails, do not ask user for another password
  if (((Kind == pkPassword) || (Kind == pkKeybInteractive)) && (!FData.Password.isEmpty() || FExtras.EmptyPassword) && !Prompts.isEmpty())
  {
    return false;
  }

  PrintAuthenticating();
  // TConsoleRunner::ScriptTerminalPromptUser
  if (!Instructions.isEmpty())
  {
    FScript->PrintMessage(Instructions);
  }
  bool Result = true;
  for (Prompt & P : Prompts)
  {
    QString Text = P.Text;
    if (!Text.isEmpty() && !Text.endsWith(QLatin1Char(' ')))
    {
      Text += QLatin1Char(' ');
    }
    qsizetype Amp = Text.indexOf(QLatin1Char('&'));
    if (Amp >= 0)
    {
      Text.remove(Amp, 1);
    }
    FScript->Print(Text);
    QString AResult;
    Result = DoInput(AResult, P.Echo, FScript->InputTimeout());
    if (!Result)
    {
      break;
    }
    P.Result = AResult;
  }
  return Result;
}

void ConsoleUserInterface::DisplayBanner(const QString & /*SessionName*/, const QString & Banner)
{
  FScript->PrintMessage(Banner);
}

unsigned int ConsoleUserInterface::QueryUser(const QString & AQuery, const QString & MoreMessages, unsigned int Answers)
{
  std::vector<QueryButton> Buttons;
  for (const auto & Item : AnswerCaptions)
  {
    if (Answers & Item.Answer)
    {
      Buttons.push_back(QueryButton{Item.Answer, QString(), nullptr});
    }
  }
  return Query(AQuery, MoreMessages, Buttons, QueryParams());
}

unsigned int ConsoleUserInterface::QueryUserEx(const QString & AQuery, const QString & MoreMessages, unsigned int Answers,
  const QueryUserParams & Params)
{
  // TConsoleRunner::ScriptTerminalQueryUser with TQueryParams: aliases
  // replace the captions of their buttons (grouped drop-down items become
  // buttons of their own on a console)
  std::vector<QueryButton> Buttons;
  for (const auto & Item : AnswerCaptions)
  {
    if (Answers & Item.Answer)
    {
      Buttons.push_back(QueryButton{Item.Answer, QString(), nullptr});
    }
  }
  for (const QueryButtonAlias & Alias : Params.Aliases)
  {
    for (QueryButton & B : Buttons)
    {
      if ((B.Answer == Alias.Button) && !Alias.Alias.isEmpty())
      {
        B.Caption = Alias.Alias;
      }
    }
  }
  QueryParams QParams;
  QParams.WaitInBatch = (Params.Params & qpWaitInBatch) != 0;
  QParams.NoBatchAnswers = Params.NoBatchAnswers;
  QParams.Timeout = Params.Timeout;
  QParams.TimeoutAnswer = Params.TimeoutAnswer;
  QParams.FatalAbort = (Params.Params & qpFatalAbort) != 0;
  return Query(AQuery, MoreMessages, Buttons, QParams);
}

unsigned int ConsoleUserInterface::Query(const QString & AQuery, const QString & MoreMessages,
  const std::vector<QueryButton> & Buttons, const QueryParams & Params)
{
  unsigned int Answers = 0;
  for (const QueryButton & B : Buttons)
  {
    Answers |= B.Answer;
  }
  unsigned int Timeout = 0;
  unsigned int TimeoutA = 0;
  if ((Params.Timeout > 0) && (Params.TimeoutAnswer != 0))
  {
    Timeout = Params.Timeout;
    TimeoutA = Params.TimeoutAnswer;
  }
  else if (Params.WaitInBatch)
  {
    Timeout = FScript->InputTimeout();
    if (Timeout != 0)
    {
      TimeoutA = AbortAnswer(Answers & ~Params.NoBatchAnswers);
    }
  }

  QString Query = AQuery;
  if (Params.FatalAbort)
  {
    // WARN_FATAL_ERROR
    Query = QStringLiteral("%1\n \nWarning: Aborting this operation will close connection!").arg(Query);
  }
  Query.replace(QStringLiteral("**"), QString());
  Query.replace(QLatin1Char('\t'), QLatin1Char(' '));
  FScript->PrintMessage(Query);
  if (!MoreMessages.isEmpty())
  {
    QString More = MoreMessages;
    More.replace(QLatin1Char('\t'), QLatin1Char(' '));
    FScript->PrintMessage(More);
  }

  QStringList Captions;
  for (const QueryButton & B : Buttons)
  {
    QString Caption = B.Caption;
    if (Caption.isEmpty())
    {
      for (const auto & Item : AnswerCaptions)
      {
        if (Item.Answer == B.Answer)
        {
          Caption = QString::fromLatin1(Item.Caption);
        }
      }
    }
    Captions.append(Caption);
  }

  // Accelerators, as in TConsoleRunner::ScriptTerminalQueryUser
  QString Accels;
  for (QString & Caption : Captions)
  {
    qsizetype P = Caption.indexOf(QLatin1Char('&'));
    if ((P >= 0) && (P + 1 < Caption.size()))
    {
      QChar Accel = Caption[P + 1].toUpper();
      if (Accels.contains(Accel))
      {
        Caption.remove(P, 1);
        Accels += QLatin1Char(' ');
      }
      else
      {
        Accels += Accel;
      }
    }
    else
    {
      Accels += QLatin1Char(' ');
    }
  }
  unsigned int CancelA = CancelAnswer(Answers);
  unsigned int AbortA = AbortAnswer(Answers & ~Params.NoBatchAnswers);
  unsigned int ContinueA = ContinueAnswer(Answers & ~Params.NoBatchAnswers);
  int CancelIndex = -1;
  int AbortIndex = -1;
  int ContinueIndex = -1;
  int TimeoutIndex = 0;
  int NumberAccel = 0;
  for (int Index = 0; Index < static_cast<int>(Captions.size()); Index++)
  {
    QString & Caption = Captions[Index];
    for (int Pass = 0; (Pass < 2) && (Accels[Index] == QLatin1Char(' ')); Pass++)
    {
      for (qsizetype Index2 = 0; Index2 < Caption.size(); Index2++)
      {
        QChar C = Caption[Index2].toUpper();
        bool Usable = (Pass == 0) ? C.isLetter() : (C != QLatin1Char(' '));
        if (Usable && !Accels.contains(C))
        {
          Caption.insert(Index2, QLatin1Char('&'));
          Accels[Index] = C;
          break;
        }
      }
    }
    if (Accels[Index] == QLatin1Char(' '))
    {
      NumberAccel++;
      Caption = QStringLiteral("&%1%2").arg(NumberAccel).arg(Caption);
      Accels[Index] = Caption[1];
    }
    if (Buttons[Index].Answer == CancelA) CancelIndex = Index + 1;
    if (Buttons[Index].Answer == AbortA) AbortIndex = Index + 1;
    if (Buttons[Index].Answer == ContinueA) ContinueIndex = Index + 1;
    if ((TimeoutA != 0) && (Buttons[Index].Answer == TimeoutA)) TimeoutIndex = Index + 1;
  }
  if (AbortIndex < 0)
  {
    AbortIndex = (CancelIndex > 0) ? CancelIndex : static_cast<int>(Buttons.size());
  }
  if (ContinueIndex < 0)
  {
    ContinueIndex = AbortIndex;
  }
  bool Timeouting = (Timeout > 0) && (TimeoutIndex > 0);
  if (!Timeouting)
  {
    Timeout = 0;
  }

  unsigned int Answer = 0;
  bool FirstOutput = true;
  do
  {
    Script::BatchMode Batch = FScript->Batch();
    int AnswerIndex = 0;
    bool Retry;
    do
    {
      Retry = false;
      // The countdown is refreshed in place on a terminal (cfLiveOutput)
      if (FirstOutput || Console::Instance().OutputIsTty())
      {
        QString Output;
        for (int Index = 0; Index < static_cast<int>(Captions.size()); Index++)
        {
          if (Index > 0)
          {
            Output += QStringLiteral(", ");
          }
          QString Caption = Captions[Index];
          qsizetype P = Caption.indexOf(QLatin1Char('&'));
          if (P >= 0)
          {
            Caption[P] = QLatin1Char('(');
            Caption.insert(P + 2, QLatin1Char(')'));
          }
          if (Timeouting && (Index + 1 == TimeoutIndex))
          {
            // TIMEOUT_BUTTON
            Caption = QStringLiteral("%1 (%2 s)").arg(Caption).arg(Timeout / 1000);
          }
          Output += Caption;
        }
        Output += QStringLiteral(": ");
        if (FirstOutput)
        {
          FScript->Print(Output);
        }
        else
        {
          Console::Instance().Print(Output, true);
        }
        FirstOutput = false;
      }

      if (!Timeouting && (Batch == Script::BatchContinue))
      {
        AnswerIndex = ContinueIndex;
      }
      else if (!Timeouting && (Batch != Script::BatchOff))
      {
        AnswerIndex = AbortIndex;
      }
      else if (Timeouting && ((Timeout < 1000) ||
               ((Batch != Script::BatchOff) && !Console::Instance().InputIsTty())))
      {
        // NixSCP: never wait for (or consume) a non-interactive stdin in batch mode
        AnswerIndex = TimeoutIndex;
      }
      else
      {
        unsigned int ActualTimer = Timeouting ? (Console::Instance().NoInteractiveInput() ? Timeout : 1000) : 0;
        AnswerIndex = Console::Instance().Choice(Accels, CancelIndex, -1, -2, ActualTimer);
        if (AnswerIndex == -1)
        {
          FScript->NotifyAbort();
          AnswerIndex = AbortIndex;
        }
        else if (AnswerIndex == -2)
        {
          if (Timeouting && (Timeout > ActualTimer))
          {
            Timeout -= ActualTimer;
            Retry = true;
          }
          else
          {
            AnswerIndex = (TimeoutIndex > 0) ? TimeoutIndex : AbortIndex;
          }
        }
        else if (AnswerIndex <= 0)
        {
          AnswerIndex = AbortIndex;
        }
      }
    }
    while (Retry);

    QString AnswerCaption = Captions[AnswerIndex - 1];
    AnswerCaption.remove(AnswerCaption.indexOf(QLatin1Char('&')), 1);
    FScript->PrintLine(AnswerCaption);
    FirstOutput = true;

    const QueryButton & Button = Buttons[AnswerIndex - 1];
    if (Button.OnSubmit && !Button.OnSubmit())
    {
      Answer = 0;
      // the answer was consumed by the alias (e.g. copy key), ask again
      // unless running in batch mode, where the same answer would repeat
      if ((Batch != Script::BatchOff) && !Timeouting)
      {
        Answer = AbortA;
      }
    }
    else
    {
      Answer = Button.Answer;
    }
  }
  while (Answer == 0);

  if (Answer == AbortA)
  {
    FScript->SetCommandError();
  }
  return Answer;
}
