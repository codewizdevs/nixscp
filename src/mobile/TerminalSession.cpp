#include "mobile/TerminalSession.h"

#include "core/FileSystem.h"
#include "core/SecureShell.h"

#include <QClipboard>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QHash>
#include <QMetaObject>

#include <chrono>

#include <libssh/libssh.h>

namespace
{
const int ScrollbackLines = 2000;
std::atomic<int> EndingShells{0};

// Declared after the connection: a channel must go before its session,
// which frees what still belongs to it
struct ChannelHolder
{
  ssh_channel Channel = nullptr;
  ~ChannelHolder()
  {
    if (Channel != nullptr)
    {
      ssh_channel_free(Channel);
    }
  }
};
}

// libvterm's callbacks as members (the user pointer is the session)
int TerminalSession::OnDamage(VTermRect, void * User)
{
  emit static_cast<TerminalSession *>(User)->updated();
  return 1;
}

int TerminalSession::OnMoveCursor(VTermPos Pos, VTermPos, int Visible, void * User)
{
  auto * Self = static_cast<TerminalSession *>(User);
  Self->FCursor = Pos;
  Self->FCursorVisible = Visible;
  emit Self->updated();
  return 1;
}

int TerminalSession::OnTermProp(VTermProp Prop, VTermValue * Value, void * User)
{
  auto * Self = static_cast<TerminalSession *>(User);
  if (Prop == VTERM_PROP_CURSORVISIBLE)
  {
    Self->FCursorVisible = Value->boolean;
    emit Self->updated();
  }
  return 1;
}

int TerminalSession::OnPushLine(int Cols, const VTermScreenCell * Cells, void * User)
{
  auto * Self = static_cast<TerminalSession *>(User);
  Self->FScrollback.append(QVector<VTermScreenCell>(Cells, Cells + Cols));
  while (Self->FScrollback.size() > ScrollbackLines)
  {
    Self->FScrollback.removeFirst();
  }
  return 1;
}

int TerminalSession::OnPopLine(int Cols, VTermScreenCell * Cells, void * User)
{
  auto * Self = static_cast<TerminalSession *>(User);
  if (Self->FScrollback.isEmpty())
  {
    return 0;
  }
  QVector<VTermScreenCell> Line = Self->FScrollback.takeLast();
  for (int I = 0; I < Cols; I++)
  {
    if (I < Line.size())
    {
      Cells[I] = Line.at(I);
    }
    else
    {
      Cells[I] = VTermScreenCell{};
      Cells[I].width = 1;
    }
  }
  return 1;
}

void TerminalSession::OnOutput(const char * Data, size_t Size, void * User)
{
  static_cast<TerminalSession *>(User)->Write(Data, Size);
}

TerminalSession::TerminalSession(const SessionData & Data, UserInterface * UI, QObject * Parent) :
  QObject(Parent), FData(Data), FUI(UI)
{
  static VTermScreenCallbacks ScreenCallbacks{};
  ScreenCallbacks.damage = &TerminalSession::OnDamage;
  ScreenCallbacks.movecursor = &TerminalSession::OnMoveCursor;
  ScreenCallbacks.settermprop = &TerminalSession::OnTermProp;
  ScreenCallbacks.sb_pushline = &TerminalSession::OnPushLine;
  ScreenCallbacks.sb_popline = &TerminalSession::OnPopLine;

  FVTerm = vterm_new(FRows, FCols);
  vterm_set_utf8(FVTerm, 1);
  vterm_output_set_callback(FVTerm, &TerminalSession::OnOutput, this);
  FState = vterm_obtain_state(FVTerm);
  FScreen = vterm_obtain_screen(FVTerm);
  vterm_screen_set_callbacks(FScreen, &ScreenCallbacks, this);
  vterm_screen_enable_altscreen(FScreen, 1);
  vterm_screen_enable_reflow(FScreen, true);
  vterm_screen_set_damage_merge(FScreen, VTERM_DAMAGE_SCROLL);
  VTermColor Fg;
  VTermColor Bg;
  vterm_color_rgb(&Fg, 0xd4, 0xd4, 0xd4);
  vterm_color_rgb(&Bg, 0x1e, 0x1e, 0x1e);
  vterm_state_set_default_colors(FState, &Fg, &Bg);
  vterm_screen_reset(FScreen, 1);
}

TerminalSession::~TerminalSession()
{
  FStop = true;
  Join();
  vterm_free(FVTerm);
}

void TerminalSession::Join()
{
  if (FThread.joinable())
  {
    FThread.join();
  }
}

void TerminalSession::Start(int Cols, int Rows)
{
  if (FRunning || FConnecting)
  {
    return;
  }
  Join();
  FStop = false;
  // what was typed into the ended shell is not for this one
  {
    QMutexLocker Lock(&FMutex);
    FOutput.clear();
  }
  Resize(Cols, Rows);
  FConnecting = true;
  emit runningChanged();
  FThread = std::thread([this, Cols, Rows]() { Run(Cols, Rows); });
}

void TerminalSession::stop()
{
  FStop = true;
}

void TerminalSession::Dispose()
{
  FStop = true;
  if (FRunning || FConnecting)
  {
    // the worker's last call deletes it
    FDisposed = true;
    EndingShells++;
  }
  else
  {
    deleteLater();
  }
}

int TerminalSession::Ending()
{
  return EndingShells;
}

void TerminalSession::Resize(int Cols, int Rows)
{
  Cols = qMax(10, Cols);
  Rows = qMax(4, Rows);
  if ((Cols == FCols) && (Rows == FRows))
  {
    return;
  }
  FCols = Cols;
  FRows = Rows;
  vterm_set_size(FVTerm, Rows, Cols);
  vterm_screen_flush_damage(FScreen);
  {
    QMutexLocker Lock(&FMutex);
    FResizeCols = Cols;
    FResizeRows = Rows;
  }
  emit updated();
}

void TerminalSession::Write(const char * Data, size_t Size)
{
  // no shell, or none coming: keys typed now must not run in the next one
  if (!FRunning && !FConnecting)
  {
    return;
  }
  QMutexLocker Lock(&FMutex);
  FOutput.append(Data, static_cast<qsizetype>(Size));
}

void TerminalSession::sendText(const QString & Text, bool Ctrl, bool Alt)
{
  VTermModifier Mod = static_cast<VTermModifier>((Ctrl ? VTERM_MOD_CTRL : 0) | (Alt ? VTERM_MOD_ALT : 0));
  const QList<uint> Chars = Text.toUcs4();
  for (uint Char : Chars)
  {
    if (Char == '\n')
    {
      vterm_keyboard_key(FVTerm, VTERM_KEY_ENTER, Mod);
    }
    else
    {
      vterm_keyboard_unichar(FVTerm, Char, Mod);
    }
  }
}

void TerminalSession::sendKey(const QString & Name, bool Ctrl, bool Alt)
{
  static const QHash<QString, VTermKey> Keys = {
    {QStringLiteral("Enter"), VTERM_KEY_ENTER}, {QStringLiteral("Backspace"), VTERM_KEY_BACKSPACE},
    {QStringLiteral("Tab"), VTERM_KEY_TAB}, {QStringLiteral("Escape"), VTERM_KEY_ESCAPE},
    {QStringLiteral("Up"), VTERM_KEY_UP}, {QStringLiteral("Down"), VTERM_KEY_DOWN},
    {QStringLiteral("Left"), VTERM_KEY_LEFT}, {QStringLiteral("Right"), VTERM_KEY_RIGHT},
    {QStringLiteral("Home"), VTERM_KEY_HOME}, {QStringLiteral("End"), VTERM_KEY_END},
    {QStringLiteral("PageUp"), VTERM_KEY_PAGEUP}, {QStringLiteral("PageDown"), VTERM_KEY_PAGEDOWN},
    {QStringLiteral("Insert"), VTERM_KEY_INS}, {QStringLiteral("Delete"), VTERM_KEY_DEL}};
  VTermModifier Mod = static_cast<VTermModifier>((Ctrl ? VTERM_MOD_CTRL : 0) | (Alt ? VTERM_MOD_ALT : 0));
  if (Name.startsWith(QLatin1Char('F')) && (Name.size() > 1) && Name.mid(1).toInt() > 0)
  {
    vterm_keyboard_key(FVTerm, static_cast<VTermKey>(VTERM_KEY_FUNCTION(Name.mid(1).toInt())), Mod);
    return;
  }
  auto It = Keys.find(Name);
  if (It != Keys.end())
  {
    vterm_keyboard_key(FVTerm, It.value(), Mod);
  }
}

void TerminalSession::paste()
{
  QString Text = QGuiApplication::clipboard()->text();
  if (Text.isEmpty())
  {
    return;
  }
  vterm_keyboard_start_paste(FVTerm);
  sendText(Text.replace(QStringLiteral("\r\n"), QStringLiteral("\n")));
  vterm_keyboard_end_paste(FVTerm);
}

void TerminalSession::Received(const QByteArray & Data)
{
  vterm_input_write(FVTerm, Data.constData(), static_cast<size_t>(Data.size()));
  vterm_screen_flush_damage(FScreen);
  FUnprocessed -= Data.size();
  emit updated();
}

// The worker thread: the SSH connection, the shell channel, reading and writing
void TerminalSession::Run(int Cols, int Rows)
{
  QString Error;
  try
  {
    SecureShell Shell(FData, FUI);
    Shell.Open();
    if (FStop)
    {
      throw AbortException();
    }
    ssh_session Session = Shell.Session();
    ChannelHolder Holder;
    ssh_channel Channel = Holder.Channel = ssh_channel_new(Session);
    if ((Channel == nullptr) || (ssh_channel_open_session(Channel) != SSH_OK))
    {
      throw ExtException(QStringLiteral("The server refused to open a session channel."));
    }
    if ((ssh_channel_request_pty_size(Channel, "xterm-256color", Cols, Rows) != SSH_OK) ||
        (ssh_channel_request_shell(Channel) != SSH_OK))
    {
      throw ExtException(QStringLiteral("The server refused to start a shell."),
        QString::fromUtf8(ssh_get_error(Session)));
    }
    QMetaObject::invokeMethod(this, [this]()
    {
      FConnecting = false;
      FRunning = true;
      emit runningChanged();
    }, Qt::QueuedConnection);

    // the keepalive of the connection: its timer needs an event loop, which this thread has not
    int PingInterval = (FData.PingType != ptOff) ? FData.PingInterval : 0;
    QElapsedTimer SincePing;
    SincePing.start();
    char Buffer[16384];
    while (!FStop)
    {
      if ((PingInterval > 0) && SincePing.hasExpired(qint64(PingInterval) * 1000))
      {
        Shell.KeepAlive();
        SincePing.restart();
      }
      QByteArray Output;
      int ResizeCols = 0;
      int ResizeRows = 0;
      {
        QMutexLocker Lock(&FMutex);
        Output.swap(FOutput);
        ResizeCols = FResizeCols;
        ResizeRows = FResizeRows;
        FResizeCols = FResizeRows = 0;
      }
      if (ResizeCols > 0)
      {
        ssh_channel_change_pty_size(Channel, ResizeCols, ResizeRows);
      }
      if (!Output.isEmpty() && (ssh_channel_write(Channel, Output.constData(), static_cast<uint32_t>(Output.size())) == SSH_ERROR))
      {
        break;
      }
      // the screen is behind ("yes", a large file): no more than it can draw
      if (FUnprocessed >= MaxUnprocessed)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      QByteArray Data;
      for (int StdErr = 0; StdErr <= 1; StdErr++)
      {
        int Read = ssh_channel_read_timeout(Channel, Buffer, sizeof(Buffer), StdErr, StdErr ? 0 : 30);
        if (Read > 0)
        {
          Data.append(Buffer, Read);
        }
        else if (Read == SSH_ERROR)
        {
          FStop = true;
        }
      }
      if (!Data.isEmpty())
      {
        FUnprocessed += Data.size();
        QMetaObject::invokeMethod(this, [this, Data]() { Received(Data); }, Qt::QueuedConnection);
      }
      if (ssh_channel_is_eof(Channel) || !ssh_channel_is_open(Channel))
      {
        break;
      }
    }
    ssh_channel_close(Channel);
  }
  catch (const AbortException &)
  {
    // stopped, or the question was not answered: nothing to report
  }
  catch (const ExtException & E)
  {
    Error = E.MoreMessages().isEmpty() ? E.Message() : (E.Message() + QLatin1Char('\n') + E.MoreMessages());
  }
  catch (const std::exception & E)
  {
    Error = QString::fromUtf8(E.what());
  }
  QMetaObject::invokeMethod(this, [this, Error]()
  {
    FConnecting = false;
    FRunning = false;
    if (FDisposed)
    {
      // nobody is listening any more; ~TerminalSession joins the worker, which is at its end
      EndingShells--;
      deleteLater();
      return;
    }
    emit runningChanged();
    emit finished(Error);
  }, Qt::QueuedConnection);
}
