// Progress window tests (WP11): src/gui/ProgressDialog (TProgressForm) and
// src/gui/SystemActions: "NN% Uploading" caption mirrored into the owner
// window title, marquee and Start time when the total size is unknown, the
// speed limit combo (History\SpeedLimit, live FileOperationProgress limit,
// mouse wheel), Skip this file (csCancelFile), Proceed in Background,
// Once Finished cycle/menu and its execution (system actions intercepted:
// the test never suspends or powers off the machine), the three-way cancel
// prompt, the sleep inhibitor reference and Delete progress.
//
// GUI test: built with NIXSCP_GUI_TESTS and the GUI objects, runs offscreen.
// NIXSCP_TEST_SCREENSHOT=<png> saves the progress window.
#ifndef NIXSCP_GUI_TESTS

#include <cstdio>

int main()
{
  std::printf("SKIP: test_progressdialog needs NIXSCP_GUI_TESTS (Qt Widgets + the GUI sources)\n");
  return 0;
}

#else

#include "core/Configuration.h"
#include "core/FileSystem.h"
#include "core/IniStorage.h"
#include "gui/ProgressDialog.h"
#include "gui/SystemActions.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QProgressBar>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QWheelEvent>

#include <cstdio>
#include <functional>

static int Failures = 0;
static int Checks = 0;

#define CHECK(Cond) \
  do { Checks++; if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_MSG(Cond, Msg) \
  do { Checks++; if (!(Cond)) { std::printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, #Cond, qPrintable(Msg)); std::fflush(stdout); Failures++; } } while (0)

static bool WaitFor(const std::function<bool()> & Condition, int Ms = 5000)
{
  QElapsedTimer Timer;
  Timer.start();
  while (!Condition())
  {
    if (Timer.elapsed() > Ms)
    {
      return false;
    }
    QApplication::processEvents(QEventLoop::AllEvents, 10);
    QThread::msleep(5);
  }
  return true;
}

static void Spin(int Ms)
{
  WaitFor([]() { return false; }, Ms);
}

static QAction * FindAction(QWidget * Parent, const QString & Text)
{
  for (QAction * Action : Parent->findChildren<QAction *>())
  {
    if (Action->text() == Text)
    {
      return Action;
    }
  }
  return nullptr;
}

static QToolButton * FindOnceDoneButton(QWidget * Parent)
{
  for (QToolButton * Button : Parent->findChildren<QToolButton *>())
  {
    if (Button->text() == QStringLiteral("Once Finished"))
    {
      return Button;
    }
  }
  return nullptr;
}

static QComboBox * FindSpeedCombo(QWidget * Parent)
{
  for (QComboBox * Combo : Parent->findChildren<QComboBox *>())
  {
    if (Combo->isEditable())
    {
      return Combo;
    }
  }
  return nullptr;
}

static QString HistoryValue(int Index)
{
  IniStorage Storage(&Configuration::Instance().Ini());
  if (Storage.OpenSubKey(QStringLiteral("Configuration"), false) &&
      Storage.OpenSubKey(QStringLiteral("History"), false) &&
      Storage.OpenSubKey(QStringLiteral("SpeedLimit"), false))
  {
    return Storage.ReadString(QString::number(Index), QString());
  }
  return QString();
}

// Answers the next modal message box with Button
static void AnswerMessageBox(QMessageBox::StandardButton Button, bool * Seen)
{
  QTimer::singleShot(50, [Button, Seen]()
  {
    auto * Box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
    if (Box == nullptr)
    {
      AnswerMessageBox(Button, Seen);
      return;
    }
    *Seen = true;
    Box->button(Button)->click();
  });
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

  QList<SystemActions::OnceDoneOperation> Executed;
  SystemActions::SetSystemActionHandler([&Executed](SystemActions::OnceDoneOperation Operation)
  {
    Executed.append(Operation);
    return true;
  });
  SystemActions::SleepInhibitor::SetBackendEnabled(false);

  // Captions
  CHECK(SystemActions::OnceDoneCaption(SystemActions::odoIdle) == QStringLiteral("&Stay Idle"));
  CHECK(SystemActions::OnceDoneCaption(SystemActions::odoSuspend) == QStringLiteral("Slee&p Computer"));
  CHECK(SystemActions::OnceDoneCaption(SystemActions::odoShutDown) == QStringLiteral("&Shut Down Computer"));

  // Sleep inhibitor reference counting
  CHECK(!SystemActions::SleepInhibitor::Held());
  {
    SystemActions::SleepInhibitLock Lock1;
    SystemActions::SleepInhibitLock Lock2;
    CHECK(SystemActions::SleepInhibitor::Held());
  }
  CHECK(!SystemActions::SleepInhibitor::Held());

  QMainWindow Window;
  Window.setWindowTitle(QStringLiteral("/home/user - site - NixSCP"));
  Window.resize(600, 400);
  Window.show();

  // ---- Upload: caption, owner title, speed limit, skip, once finished ----
  {
    FileOperationProgress Progress;
    ProgressDialog Dialog(Progress, &Window);
    Dialog.SetTarget(QStringLiteral("/remote/dir"));
    Dialog.Start();
    // taken only once the operation has run for the show delay
    CHECK(!SystemActions::SleepInhibitor::Held());
    Progress.Start(FileOperationProgress::Copy, FileOperationProgress::ToRemote, 3);
    Progress.SetCPSLimit(0);
    Progress.AddTotalSize(1000);
    Progress.SetFile(QStringLiteral("/local/a.txt"), 500);
    Progress.AddTransferred(250);
    CHECK(WaitFor([&]() { return Dialog.isVisible(); }));
    CHECK(SystemActions::SleepInhibitor::Held());
    Spin(150);
    CHECK_MSG(Dialog.windowTitle() == QStringLiteral("25% Uploading"), Dialog.windowTitle());
    CHECK(Dialog.ProgressTitle() == QStringLiteral("25% Uploading"));
    CHECK_MSG(Window.windowTitle() == QStringLiteral("25% Uploading - /home/user - site - NixSCP"), Window.windowTitle());

    // Target label
    bool TargetShown = false;
    for (QLabel * Label : Dialog.findChildren<QLabel *>())
    {
      if ((Label->text() == QStringLiteral("Target:")) && Label->isVisible())
      {
        TargetShown = true;
      }
    }
    CHECK(TargetShown);

    // Toolbar items
    QAction * Skip = FindAction(&Dialog, QStringLiteral("Skip this file"));
    QAction * Minimize = FindAction(&Dialog, QStringLiteral("&Minimize"));
    QAction * MoveToQueue = FindAction(&Dialog, QStringLiteral("Proceed in &Background"));
    QToolButton * OnceDone = FindOnceDoneButton(&Dialog);
    QComboBox * Speed = FindSpeedCombo(&Dialog);
    CHECK((Skip != nullptr) && Skip->isVisible() && Skip->isEnabled());
    CHECK((Minimize != nullptr) && (Minimize->shortcut() == QKeySequence(Qt::ALT | Qt::Key_M)));
    // no MoveToQueueRequested receiver: not offered
    CHECK((MoveToQueue != nullptr) && !MoveToQueue->isVisible());
    CHECK((MoveToQueue != nullptr) && (MoveToQueue->shortcut() == QKeySequence(Qt::ALT | Qt::Key_B)));
    CHECK((OnceDone != nullptr) && OnceDone->isVisible());
    CHECK((Speed != nullptr) && Speed->isVisible());

    // Speed combo: default history, current limit, live change and history
    if (Speed != nullptr)
    {
      CHECK(Speed->count() > 5);
      CHECK(Speed->itemText(0) == QStringLiteral("Unlimited"));
      CHECK(Speed->itemText(1) == QStringLiteral("8192"));
      CHECK_MSG(Speed->currentText() == QStringLiteral("Unlimited"), Speed->currentText());
      CHECK(Speed->toolTip() == QStringLiteral("Speed limit (KB/s)"));
      Speed->setEditText(QStringLiteral("64"));
      emit Speed->lineEdit()->returnPressed();
      CHECK(Progress.CPSLimit() == 64 * 1024);
      CHECK_MSG(HistoryValue(0) == QStringLiteral("64"), HistoryValue(0));
      // CopySpeedLimits keeps the MRU order (Unlimited is not forced first)
      CHECK_MSG(Speed->itemText(0) == QStringLiteral("64"), Speed->itemText(0));
      CHECK(Speed->findText(QStringLiteral("Unlimited")) == 1);
      // mouse wheel doubles (more than 4 notches per step, as WinSCP)
      QWheelEvent Up(QPointF(5, 5), Speed->mapToGlobal(QPointF(5, 5)), QPoint(), QPoint(0, 600),
        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
      QApplication::sendEvent(Speed, &Up);
      CHECK_MSG(Progress.CPSLimit() == 128 * 1024, QString::number(Progress.CPSLimit()));
      CHECK(Speed->currentText() == QStringLiteral("128"));
      QWheelEvent Down(QPointF(5, 5), Speed->mapToGlobal(QPointF(5, 5)), QPoint(), QPoint(0, -1200),
        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
      QApplication::sendEvent(Speed, &Down);
      CHECK_MSG(Progress.CPSLimit() == 32 * 1024, QString::number(Progress.CPSLimit()));
      // invalid text is refused and restored
      bool Seen = false;
      AnswerMessageBox(QMessageBox::Ok, &Seen);
      Speed->setEditText(QStringLiteral("fast"));
      emit Speed->lineEdit()->returnPressed();
      CHECK(Seen);
      CHECK(Progress.CPSLimit() == 32 * 1024);
      CHECK(Speed->currentText() == QStringLiteral("32"));
    }

    // Skip this file -> csCancelFile handed to the worker, pending until the next file
    if (Skip != nullptr)
    {
      Skip->trigger();
      bool Skipped = false;
      try
      {
        Progress.CheckCancel();
      }
      catch (const SkipFileException &)
      {
        Skipped = true;
      }
      catch (const AbortException &)
      {
      }
      CHECK(Skipped);
      CHECK(!Progress.Cancelled());
      Dialog.UpdateControls();
      CHECK(!Skip->isEnabled());
      Progress.SetFile(QStringLiteral("/local/b.txt"), 500);
      Dialog.UpdateControls();
      CHECK(Skip->isEnabled());
    }

    // Once Finished: click cycles (Disconnect hidden without a receiver)
    CHECK(Dialog.OnceDoneOperation() == SystemActions::odoIdle);
    if (OnceDone != nullptr)
    {
      OnceDone->click();
      CHECK(Dialog.OnceDoneOperation() == SystemActions::odoSuspend);
      OnceDone->click();
      CHECK(Dialog.OnceDoneOperation() == SystemActions::odoShutDown);
      OnceDone->click();
      CHECK(Dialog.OnceDoneOperation() == SystemActions::odoIdle);
      QList<QAction *> Items = OnceDone->menu()->actions();
      CHECK(Items.size() == 4);
      CHECK(!Items[SystemActions::odoDisconnect]->isVisible());
      Items[SystemActions::odoSuspend]->trigger();
      CHECK(Dialog.OnceDoneOperation() == SystemActions::odoSuspend);
    }

    QString Shot = qEnvironmentVariable("NIXSCP_TEST_SCREENSHOT");
    if (!Shot.isEmpty())
    {
      Dialog.grab().save(Shot);
    }

    Progress.Stop();
    Dialog.Finish();
    CHECK(!Dialog.isVisible());
    CHECK(Executed == QList<SystemActions::OnceDoneOperation>{SystemActions::odoSuspend});
    CHECK(Dialog.OnceDoneOperation() == SystemActions::odoIdle);
    CHECK_MSG(Window.windowTitle() == QStringLiteral("/home/user - site - NixSCP"), Window.windowTitle());
    CHECK(!SystemActions::SleepInhibitor::Held());
  }

  // ---- Download with unknown total: marquee, start time, receivers ----
  {
    Executed.clear();
    FileOperationProgress Progress;
    ProgressDialog Dialog(Progress, &Window);
    QStringList Titles;
    int MoveRequests = 0;
    QList<SystemActions::OnceDoneOperation> Requested;
    QObject::connect(&Dialog, &ProgressDialog::ProgressTitleChanged, [&Titles](const QString & Title) { Titles.append(Title); });
    QObject::connect(&Dialog, &ProgressDialog::MoveToQueueRequested, [&MoveRequests]() { MoveRequests++; });
    QObject::connect(&Dialog, &ProgressDialog::OnceDoneRequested,
      [&Requested](SystemActions::OnceDoneOperation Operation) { Requested.append(Operation); });
    Dialog.Start();
    Progress.Start(FileOperationProgress::Copy, FileOperationProgress::ToLocal, 1);
    Progress.SetFile(QStringLiteral("/remote/big.iso"), 1000);
    Progress.AddTransferred(10);
    CHECK(WaitFor([&]() { return Dialog.isVisible(); }));
    Spin(150);
    CHECK_MSG(Dialog.windowTitle() == QStringLiteral("Downloading"), Dialog.windowTitle());
    CHECK(!Titles.isEmpty() && (Titles.last() == QStringLiteral("Downloading")));
    // the dialog leaves the owner title to the receiver
    CHECK(Window.windowTitle() == QStringLiteral("/home/user - site - NixSCP"));
    QProgressBar * Overall = nullptr;
    for (QProgressBar * Bar : Dialog.findChildren<QProgressBar *>())
    {
      if (Overall == nullptr)
      {
        Overall = Bar;   // OperationProgress is created first
      }
    }
    CHECK((Overall != nullptr) && (Overall->minimum() == 0) && (Overall->maximum() == 0));
    bool StartTimeShown = false;
    bool TimeLeftShown = false;
    for (QLabel * Label : Dialog.findChildren<QLabel *>())
    {
      if (Label->text() == QStringLiteral("Start time:"))
      {
        StartTimeShown = Label->isVisible();
      }
      if (Label->text() == QStringLiteral("Time left:"))
      {
        TimeLeftShown = Label->isVisible();
      }
    }
    CHECK(StartTimeShown);
    CHECK(!TimeLeftShown);

    QAction * MoveToQueue = FindAction(&Dialog, QStringLiteral("Proceed in &Background"));
    CHECK((MoveToQueue != nullptr) && MoveToQueue->isVisible() && MoveToQueue->isEnabled());
    QToolButton * OnceDone = FindOnceDoneButton(&Dialog);
    CHECK((OnceDone != nullptr) && OnceDone->menu()->actions()[SystemActions::odoDisconnect]->isVisible());
    if (OnceDone != nullptr)
    {
      OnceDone->click();
      CHECK(Dialog.OnceDoneOperation() == SystemActions::odoDisconnect);
    }

    // Proceed in Background: the current file finishes first
    if (MoveToQueue != nullptr)
    {
      MoveToQueue->trigger();
      CHECK(MoveRequests == 1);
      CHECK(Dialog.MoveToQueue());
      CHECK(!MoveToQueue->isEnabled());
      Spin(150);
      CHECK(!Progress.Cancelled());
      Progress.AddTransferred(990);
      Progress.FileFinished();
      CHECK(WaitFor([&]() { return Progress.Cancelled(); }, 1000));
    }
    Progress.Stop();
    Dialog.Finish();
    // moved to the queue: no once-finished action
    CHECK(Requested.isEmpty());
    CHECK(Executed.isEmpty());
    CHECK(!Titles.isEmpty() && Titles.last().isEmpty());
  }

  // ---- Once Finished after success: receiver first, then the system action ----
  {
    Executed.clear();
    FileOperationProgress Progress;
    ProgressDialog Dialog(Progress, &Window);
    QList<SystemActions::OnceDoneOperation> Requested;
    QObject::connect(&Dialog, &ProgressDialog::OnceDoneRequested,
      [&Requested, &Executed](SystemActions::OnceDoneOperation Operation)
    {
      CHECK(Executed.isEmpty());
      Requested.append(Operation);
    });
    Dialog.Start();
    Dialog.SetOnceDoneOperation(SystemActions::odoShutDown);
    Progress.Start(FileOperationProgress::Copy, FileOperationProgress::ToRemote, 1);
    Progress.Stop();
    Dialog.Finish(true);
    CHECK(Requested == QList<SystemActions::OnceDoneOperation>{SystemActions::odoShutDown});
    CHECK(Executed == QList<SystemActions::OnceDoneOperation>{SystemActions::odoShutDown});

    // failed operation: nothing happens
    Requested.clear();
    Executed.clear();
    Dialog.Start();
    Dialog.SetOnceDoneOperation(SystemActions::odoSuspend);
    Progress.Start(FileOperationProgress::Copy, FileOperationProgress::ToRemote, 1);
    Progress.Stop();
    Dialog.Finish(false);
    CHECK(Requested.isEmpty());
    CHECK(Executed.isEmpty());
  }

  // ---- Three-way cancel prompt during a file transfer ----
  {
    FileOperationProgress Progress;
    ProgressDialog Dialog(Progress, &Window);
    Dialog.Start();
    Progress.Start(FileOperationProgress::Copy, FileOperationProgress::ToRemote, 2);
    Progress.AddTotalSize(2000000);
    Progress.SetFile(QStringLiteral("/local/big.bin"), 1000000);
    Progress.AddTransferred(1);
    CHECK(WaitFor([&]() { return Dialog.isVisible(); }));

    // Cancel = continue
    bool Seen = false;
    AnswerMessageBox(QMessageBox::Cancel, &Seen);
    static_cast<QDialog &>(Dialog).reject();
    CHECK(Seen);
    CHECK(Dialog.Cancel() == ProgressDialog::csContinue);
    CHECK(!Progress.Cancelled());
    CHECK(!Progress.Suspended());

    // No = finish the current file
    Seen = false;
    AnswerMessageBox(QMessageBox::No, &Seen);
    static_cast<QDialog &>(Dialog).reject();
    CHECK(Seen);
    CHECK(Dialog.Cancel() == ProgressDialog::csCancel);
    CHECK(!Progress.Cancelled());
    QAction * CancelItem = FindAction(&Dialog, QStringLiteral("&Cancel"));
    CHECK((CancelItem != nullptr) && !CancelItem->isEnabled());
    Progress.SetFile(QStringLiteral("/local/next.bin"), 1000000);
    CHECK(WaitFor([&]() { return Progress.Cancelled(); }, 1000));
    Progress.Stop();
    Dialog.Finish();
    CHECK(Executed.isEmpty());

    // Yes = cancel now
    Dialog.Start();
    Progress.Start(FileOperationProgress::Copy, FileOperationProgress::ToRemote, 2);
    Progress.AddTotalSize(2000000);
    Progress.SetFile(QStringLiteral("/local/big.bin"), 1000000);
    Progress.AddTransferred(1);
    CHECK(WaitFor([&]() { return Dialog.isVisible(); }));
    Seen = false;
    AnswerMessageBox(QMessageBox::Yes, &Seen);
    static_cast<QDialog &>(Dialog).reject();
    CHECK(Seen);
    CHECK(Dialog.Cancel() == ProgressDialog::csCancelTransfer);
    CHECK(Progress.Cancelled());
    Progress.Stop();
    Dialog.Finish();

    // not transferring a file: plain cancel, no prompt
    Dialog.Start();
    Progress.Start(FileOperationProgress::Delete, FileOperationProgress::ToRemote, 4);
    CHECK(WaitFor([&]() { return Dialog.isVisible(); }));
    static_cast<QDialog &>(Dialog).reject();
    CHECK(Progress.Cancelled());
    Progress.Stop();
    Dialog.Finish();
  }

  // ---- Delete progress ----
  {
    FileOperationProgress Progress;
    ProgressDialog Dialog(Progress, &Window);
    Dialog.Start();
    Progress.Start(FileOperationProgress::Delete, FileOperationProgress::ToRemote, 4);
    Progress.SetFile(QStringLiteral("/remote/x"), 0);
    Progress.FileFinished();
    CHECK(WaitFor([&]() { return Dialog.isVisible(); }));
    Spin(150);
    CHECK_MSG(Dialog.windowTitle() == QStringLiteral("25% Deleting"), Dialog.windowTitle());
    QAction * Skip = FindAction(&Dialog, QStringLiteral("Skip this file"));
    CHECK((Skip != nullptr) && !Skip->isVisible());
    QComboBox * Speed = FindSpeedCombo(&Dialog);
    CHECK((Speed != nullptr) && !Speed->isVisible());
    CHECK((FindOnceDoneButton(&Dialog) != nullptr) && FindOnceDoneButton(&Dialog)->isVisible());
    Progress.Stop();
    Dialog.Finish();

    // Calculate: no Once Finished
    Dialog.Start();
    Progress.Start(FileOperationProgress::Calculate, FileOperationProgress::ToRemote, 1);
    CHECK(WaitFor([&]() { return Dialog.isVisible(); }));
    Spin(150);
    CHECK(Dialog.windowTitle() == QStringLiteral("Calculating directory size"));
    CHECK(!FindOnceDoneButton(&Dialog)->isVisible());
    Progress.Stop();
    Dialog.Finish();
  }

  SystemActions::SetSystemActionHandler({});
  std::printf("%d checks, %d failures\n", Checks, Failures);
  return (Failures == 0) ? 0 : 1;
}

#endif
