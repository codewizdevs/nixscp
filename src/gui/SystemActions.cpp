// Port of the system actions of WinSCP: SuspendWindows / ShutDownWindows and
// OpenFolderInExplorer / OpenFileInExplorer (source/windows/Tools.cpp),
// TFileOperationProgressType::SystemRequired (source/core/FileOperationProgress.cpp),
// the ESshTerminate branch of ShowExtendedExceptionEx (source/windows/UserInterface.cpp)
// and the Once Finished captions of source/forms/Progress.dfm.
// Windows APIs are replaced by systemd-logind / freedesktop D-Bus services.
#include "gui/SystemActions.h"

#include "core/Configuration.h"
#include "gui/DesktopIntegration.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#ifdef NIXSCP_HAVE_QTDBUS
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusUnixFileDescriptor>
#endif

namespace SystemActions
{

namespace
{

const QString Login1Service = QStringLiteral("org.freedesktop.login1");
const QString Login1Path = QStringLiteral("/org/freedesktop/login1");
const QString Login1Manager = QStringLiteral("org.freedesktop.login1.Manager");

std::function<void(const QString &)> & ErrorHandler()
{
  static std::function<void(const QString &)> Handler;
  return Handler;
}

std::function<bool(OnceDoneOperation)> & ActionHandler()
{
  static std::function<bool(OnceDoneOperation)> Handler;
  return Handler;
}

void ReportError(const QString & Error)
{
  if (ErrorHandler())
  {
    ErrorHandler()(Error);
  }
  else
  {
    qWarning("NixSCP: %s", qPrintable(Error));
  }
}

void SetError(QString * Error, const QString & Message)
{
  if (Error != nullptr)
  {
    *Error = Message;
  }
}

#ifndef NIXSCP_HAVE_QTDBUS
// Runs a command line tool asynchronously; a failure is reported through the error handler
bool RunTool(const QString & Program, const QStringList & Arguments, const QString & What, QString * Error)
{
  QString Path = QStandardPaths::findExecutable(Program);
  if (Path.isEmpty())
  {
    SetError(Error, QStringLiteral("%1: '%2' was not found.").arg(What, Program));
    return false;
  }
  auto * Process = new QProcess(qApp);
  Process->setProcessChannelMode(QProcess::MergedChannels);
  QObject::connect(Process, &QProcess::finished, Process, [Process, What](int ExitCode, QProcess::ExitStatus Status)
  {
    if ((Status != QProcess::NormalExit) || (ExitCode != 0))
    {
      QString Output = QString::fromLocal8Bit(Process->readAll()).trimmed();
      ReportError(QStringLiteral("%1 failed%2").arg(What, Output.isEmpty() ? QString() : QStringLiteral(": ") + Output));
    }
    Process->deleteLater();
  });
  QObject::connect(Process, &QProcess::errorOccurred, Process, [Process, What](QProcess::ProcessError Err)
  {
    if (Err == QProcess::FailedToStart)
    {
      ReportError(QStringLiteral("%1 failed: %2").arg(What, Process->errorString()));
      Process->deleteLater();
    }
  });
  Process->start(Path, Arguments);
  return true;
}
#endif

bool CallLogind(const QString & Method, const QString & What, QString * Error)
{
#ifdef NIXSCP_HAVE_QTDBUS
  QDBusConnection Bus = QDBusConnection::systemBus();
  if (!Bus.isConnected())
  {
    SetError(Error, QStringLiteral("%1: the system D-Bus is not available.").arg(What));
    return false;
  }
  QDBusMessage Message = QDBusMessage::createMethodCall(Login1Service, Login1Path, Login1Manager, Method);
  Message << true; // interactive: polkit may ask for authentication
  Message.setInteractiveAuthorizationAllowed(true);
  auto * Watcher = new QDBusPendingCallWatcher(Bus.asyncCall(Message, 120000), qApp);
  QObject::connect(Watcher, &QDBusPendingCallWatcher::finished, Watcher, [What](QDBusPendingCallWatcher * W)
  {
    QDBusPendingReply<> Reply = *W;
    if (Reply.isError())
    {
      ReportError(QStringLiteral("%1 failed: %2").arg(What, Reply.error().message()));
    }
    W->deleteLater();
  });
  return true;
#else
  return RunTool(QStringLiteral("busctl"),
    {QStringLiteral("call"), QStringLiteral("--system"), Login1Service, Login1Path, Login1Manager, Method,
     QStringLiteral("b"), QStringLiteral("true")}, What, Error);
#endif
}

}

QString OnceDoneCaption(OnceDoneOperation Operation)
{
  switch (Operation)
  {
    case odoIdle: return QStringLiteral("&Stay Idle");
    case odoDisconnect: return QStringLiteral("&Disconnect Session");
    case odoSuspend: return QStringLiteral("Slee&p Computer");
    case odoShutDown: return QStringLiteral("&Shut Down Computer");
  }
  return QString();
}

QString OnceDoneIconName(OnceDoneOperation Operation)
{
  switch (Operation)
  {
    case odoIdle: return QStringLiteral("queue-idle-once-empty");
    case odoDisconnect: return QStringLiteral("queue-disconnect-once-empty");
    case odoSuspend: return QStringLiteral("queue-suspend-once-empty");
    case odoShutDown: return QStringLiteral("queue-shutdown-once-empty");
  }
  return QString();
}

bool Suspend(QString * Error)
{
  if (ActionHandler())
  {
    return ActionHandler()(odoSuspend);
  }
  return CallLogind(QStringLiteral("Suspend"), QStringLiteral("Sleep Computer"), Error);
}

bool ShutDown(QString * Error)
{
  if (ActionHandler())
  {
    return ActionHandler()(odoShutDown);
  }
  return CallLogind(QStringLiteral("PowerOff"), QStringLiteral("Shut Down Computer"), Error);
}

bool Execute(OnceDoneOperation Operation, QString * Error)
{
  switch (Operation)
  {
    case odoSuspend: return Suspend(Error);
    case odoShutDown: return ShutDown(Error);
    default: return true;
  }
}

void SetErrorHandler(const std::function<void(const QString &)> & Handler)
{
  ErrorHandler() = Handler;
}

void SetSystemActionHandler(const std::function<bool(OnceDoneOperation)> & Handler)
{
  ActionHandler() = Handler;
}

void ShowFolder(const QString & Path)
{
  // FileManager1.ShowFolders (QtDBus or gdbus), else xdg-open
  DesktopIntegration::ExploreDirectory(Path);
}

void ShowFile(const QString & FileName)
{
  // FileManager1.ShowItems (the file selected), else its folder with xdg-open
  DesktopIntegration::ExploreFile(FileName);
}

bool OpenFile(const QString & FileName)
{
  return QDesktopServices::openUrl(QUrl::fromLocalFile(FileName));
}

void Notify(const QString & Title, const QString & Message)
{
  // PopupTrayBalloon: honours BalloonNotifications (else FlashOnBackground),
  // the tray balloon while minimized to the tray, org.freedesktop.Notifications
  // over QtDBus or gdbus, notify-send as the last resort
  DesktopIntegration::Notify(Title, Message);
}

bool ConfirmCloseOnCompletion(QWidget * Parent, OnceDoneOperation Operation, bool ForActiveSession,
  int OtherSessions, const QString & TargetLocalPath, const QString & DestLocalFileName)
{
  Configuration & Config = Configuration::Instance();
  bool Confirm =
    ((Operation == odoDisconnect) || (Operation == odoSuspend)) &&
    Config.ConfirmExitOnCompletion;
  if (!Confirm)
  {
    return true;
  }

  // CLOSED_ON_COMPLETION, MainInstructions
  QString Text = QStringLiteral("Operation was successfully completed. Connection was closed.");
  QMessageBox Box(Parent);
  Box.setWindowTitle(QApplication::applicationDisplayName().isEmpty() ?
    QStringLiteral("NixSCP") : QApplication::applicationDisplayName());
  Box.setIcon(QMessageBox::Information);
  Box.setText(Text);
  if (ForActiveSession)
  {
    Box.setInformativeText((OtherSessions > 0) ?
      QStringLiteral("Do you want to terminate %1 remaining session(s) and close the application?").arg(OtherSessions) :
      QStringLiteral("Do you want to close application?"));
    Box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    Box.setDefaultButton(QMessageBox::Yes);
  }
  else
  {
    Box.setStandardButtons(QMessageBox::Ok);
  }

  // OPEN_BUTTON with OPEN_TARGET_FOLDER / OPEN_DOWNLOADED_FILE (TOpenLocalPathHandler)
  QMenu OpenMenu;
  if (!TargetLocalPath.isEmpty() && !ForActiveSession)
  {
    auto * OpenButton = new QPushButton(QStringLiteral("&Open"));
    QAction * FolderAction = OpenMenu.addAction(QStringLiteral("Open &Target Folder"));
    QObject::connect(FolderAction, &QAction::triggered, [TargetLocalPath, DestLocalFileName]()
    {
      if (DestLocalFileName.isEmpty())
      {
        ShowFolder(TargetLocalPath);
      }
      else
      {
        ShowFile(DestLocalFileName);
      }
    });
    if (!DestLocalFileName.isEmpty())
    {
      QAction * FileAction = OpenMenu.addAction(QStringLiteral("Open &Downloaded File"));
      QObject::connect(FileAction, &QAction::triggered, [DestLocalFileName]() { OpenFile(DestLocalFileName); });
    }
    OpenButton->setMenu(&OpenMenu);
    Box.addButton(OpenButton, QMessageBox::ActionRole);
  }

  auto * NeverAsk = new QCheckBox(QStringLiteral("Never ask me a&gain"));
  Box.setCheckBox(NeverAsk);
  int Result = Box.exec();
  if (NeverAsk->isChecked())
  {
    // qaNeverAskAgain -> qaYes
    Config.ConfirmExitOnCompletion = false;
    Config.Save();
    return true;
  }
  // ShowExtendedExceptionEx: only qaYes terminates the application (qaOK of
  // the inactive-session variant just acknowledges)
  return (Result == QMessageBox::Yes);
}

//---------------------------------------------------------------------------
// SleepInhibitor

namespace
{

struct InhibitorState
{
  int Count = 0;
  bool BackendEnabled = true;
  bool Locked = false;
  unsigned int Generation = 0;
#ifdef NIXSCP_HAVE_QTDBUS
  QDBusUnixFileDescriptor Fd;
#else
  QPointer<QProcess> Process;
#endif
};

InhibitorState & Inhibitor()
{
  static InhibitorState State;
  return State;
}

void TakeLock()
{
  InhibitorState & S = Inhibitor();
  S.Locked = true;
  unsigned int Generation = ++S.Generation;
#ifdef NIXSCP_HAVE_QTDBUS
  QDBusConnection Bus = QDBusConnection::systemBus();
  if (!Bus.isConnected())
  {
    S.Locked = false;
    return;
  }
  QDBusMessage Message = QDBusMessage::createMethodCall(Login1Service, Login1Path, Login1Manager, QStringLiteral("Inhibit"));
  Message << QStringLiteral("sleep:idle") << QStringLiteral("NixSCP") << QStringLiteral("Transfer in progress")
    << QStringLiteral("block");
  auto * Watcher = new QDBusPendingCallWatcher(Bus.asyncCall(Message), qApp);
  QObject::connect(Watcher, &QDBusPendingCallWatcher::finished, Watcher, [Generation](QDBusPendingCallWatcher * W)
  {
    QDBusPendingReply<QDBusUnixFileDescriptor> Reply = *W;
    InhibitorState & S = Inhibitor();
    if ((Generation == S.Generation) && S.Locked)
    {
      if (Reply.isError())
      {
        S.Locked = false;
      }
      else
      {
        // the lock lasts as long as the descriptor is open
        S.Fd = Reply.value();
      }
    }
    W->deleteLater();
  });
#else
  Q_UNUSED(Generation);
  QString Program = QStandardPaths::findExecutable(QStringLiteral("systemd-inhibit"));
  QString Cat = QStandardPaths::findExecutable(QStringLiteral("cat"));
  if (Program.isEmpty() || Cat.isEmpty())
  {
    S.Locked = false;
    return;
  }
  // systemd-inhibit holds the logind lock while its child runs; "cat" exits
  // when we close its stdin (or when NixSCP dies), which releases the lock
  auto * Process = new QProcess(qApp);
  Process->setProcessChannelMode(QProcess::ForwardedErrorChannel);
  Process->setStandardOutputFile(QProcess::nullDevice());
  QObject::connect(Process, &QProcess::finished, Process, [Process]()
  {
    InhibitorState & S = Inhibitor();
    if (S.Process == Process)
    {
      S.Locked = false;
      S.Process = nullptr;
    }
    Process->deleteLater();
  });
  QObject::connect(Process, &QProcess::errorOccurred, Process, [Process](QProcess::ProcessError Err)
  {
    if (Err == QProcess::FailedToStart)
    {
      InhibitorState & S = Inhibitor();
      if (S.Process == Process)
      {
        S.Locked = false;
        S.Process = nullptr;
      }
      Process->deleteLater();
    }
  });
  S.Process = Process;
  Process->start(Program,
    {QStringLiteral("--what=sleep:idle"), QStringLiteral("--who=NixSCP"), QStringLiteral("--why=Transfer in progress"),
     QStringLiteral("--mode=block"), Cat});
#endif
}

void DropLock()
{
  InhibitorState & S = Inhibitor();
  S.Locked = false;
  S.Generation++;
#ifdef NIXSCP_HAVE_QTDBUS
  S.Fd = QDBusUnixFileDescriptor();
#else
  if (S.Process)
  {
    QProcess * Process = S.Process;
    S.Process = nullptr;
    // cat exits on EOF, which ends systemd-inhibit and releases the lock; no
    // blocking wait on the GUI thread (the process deletes itself on finish)
    QObject::connect(Process, &QProcess::started, Process, [Process]() { Process->closeWriteChannel(); });
    Process->closeWriteChannel();
    // terminate in case cat did not get the EOF (systemd-inhibit forwards
    // SIGTERM to its child)
    QTimer::singleShot(2000, Process, [Process]()
    {
      if (Process->state() != QProcess::NotRunning)
      {
        Process->terminate();
      }
    });
  }
#endif
}

}

void SleepInhibitor::Acquire()
{
  InhibitorState & S = Inhibitor();
  S.Count++;
  if ((S.Count == 1) && S.BackendEnabled)
  {
    TakeLock();
  }
}

void SleepInhibitor::Release()
{
  InhibitorState & S = Inhibitor();
  if (S.Count <= 0)
  {
    return;
  }
  S.Count--;
  if (S.Count == 0)
  {
    DropLock();
  }
}

bool SleepInhibitor::Held()
{
  return Inhibitor().Count > 0;
}

bool SleepInhibitor::LockActive()
{
  return Inhibitor().Locked;
}

void SleepInhibitor::SetBackendEnabled(bool Enabled)
{
  Inhibitor().BackendEnabled = Enabled;
}

}
