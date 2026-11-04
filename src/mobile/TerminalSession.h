// An interactive shell on the server, as in PuTTY: its own SSH connection
// (run on its own thread, as the tunnels), a pseudo-terminal
// "xterm-256color" (top, htop, vi, less work) and libvterm for the screen
// and the scrollback. TerminalView draws it; the keys come from QML.
#pragma once

#include "core/SessionData.h"

#include <QByteArray>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QVector>

#include <atomic>
#include <thread>

#include <vterm.h>

class UserInterface;

class TerminalSession : public QObject
{
  Q_OBJECT
  Q_PROPERTY(bool running READ running NOTIFY runningChanged)
  // the connection is being opened
  Q_PROPERTY(bool connecting READ connecting NOTIFY runningChanged)

public:
  TerminalSession(const SessionData & Data, UserInterface * UI, QObject * Parent = nullptr);
  ~TerminalSession() override;

  bool running() const { return FRunning; }
  bool connecting() const { return FConnecting; }

  // connects with a Cols x Rows terminal
  void Start(int Cols, int Rows);
  Q_INVOKABLE void stop();
  // typed text; Ctrl / Alt: the modifiers of the extra keys
  Q_INVOKABLE void sendText(const QString & Text, bool Ctrl = false, bool Alt = false);
  // "Enter", "Backspace", "Tab", "Escape", "Up", "Down", "Left", "Right",
  // "Home", "End", "PageUp", "PageDown", "Insert", "Delete", "F1".."F12"
  Q_INVOKABLE void sendKey(const QString & Name, bool Ctrl = false, bool Alt = false);
  // the clipboard's text
  Q_INVOKABLE void paste();

  // ---- for TerminalView (GUI thread)
  void Resize(int Cols, int Rows);
  int Cols() const { return FCols; }
  int Rows() const { return FRows; }
  VTermScreen * Screen() const { return FScreen; }
  VTermState * State() const { return FState; }
  VTermPos Cursor() const { return FCursor; }
  bool CursorVisible() const { return FCursorVisible; }
  int ScrollbackCount() const { return static_cast<int>(FScrollback.size()); }
  // 0 = the oldest line
  const QVector<VTermScreenCell> & ScrollbackLine(int Index) const { return FScrollback.at(Index); }

signals:
  void runningChanged();
  // the screen changed
  void updated();
  // the shell ended (Message: why, empty when it exited normally)
  void finished(const QString & message);

private:
  SessionData FData;
  UserInterface * FUI;
  VTerm * FVTerm = nullptr;
  VTermScreen * FScreen = nullptr;
  VTermState * FState = nullptr;
  int FCols = 80;
  int FRows = 24;
  VTermPos FCursor{0, 0};
  bool FCursorVisible = true;
  QList<QVector<VTermScreenCell>> FScrollback;
  bool FRunning = false;
  bool FConnecting = false;

  // the worker thread and what it shares
  std::thread FThread;
  std::atomic<bool> FStop{false};
  QMutex FMutex;
  QByteArray FOutput;         // to the server
  int FResizeCols = 0;        // pending size change
  int FResizeRows = 0;

  void Run(int Cols, int Rows);
  void Received(const QByteArray & Data);
  void Write(const char * Data, size_t Size);
  void Join();

  static int OnDamage(VTermRect Rect, void * User);
  static int OnMoveCursor(VTermPos Pos, VTermPos OldPos, int Visible, void * User);
  static int OnTermProp(VTermProp Prop, VTermValue * Value, void * User);
  static int OnPushLine(int Cols, const VTermScreenCell * Cells, void * User);
  static int OnPopLine(int Cols, VTermScreenCell * Cells, void * User);
  static void OnOutput(const char * Data, size_t Size, void * User);
};
