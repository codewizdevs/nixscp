// Queue panel tests (WP09): src/gui/QueuePanel (QueueView3 columns and rows,
// QueueLabel "Queue (N)", toolbar and QueuePopup contents, action states of
// TQueueController::AllowOperation, Enter/Delete keys, Once Empty cycle,
// QueueView\Show visibility modes, column layout persistence, window
// shortcuts Ctrl+Q / Ctrl+Shift+Q, QueueAutoPopup of a pending error).
//
// The GUI part needs Qt Widgets and the GUI sources, which the default test
// targets do not link (they link nixscpcore only). Build with
// -DNIXSCP_GUI_TESTS, the objects of the nixscp target (except main.cpp) and
// Qt6::Widgets to run it; otherwise the test only reports that it was skipped.
// Runs offscreen (QT_QPA_PLATFORM=offscreen is set when unset). No server is
// needed: the session points to a closed port, so items wait (processing
// disabled) or fail to connect. NIXSCP_TEST_SCREENSHOT=<png> saves the panel.
#ifndef NIXSCP_GUI_TESTS

#include <cstdio>

int main()
{
  std::printf("SKIP: test_queuepanel needs NIXSCP_GUI_TESTS (Qt Widgets + the GUI sources)\n");
  return 0;
}

#else

#include "core/Configuration.h"
#include "core/IniStorage.h"
#include "core/Queue.h"
#include "gui/QueuePanel.h"

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QToolBar>
#include <QTreeWidget>
#include <QWidgetAction>

#include <atomic>
#include <cstdio>
#include <functional>

static int Failures = 0;
static int Checks = 0;

#define CHECK(Cond) \
  do { Checks++; if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_MSG(Cond, Msg) \
  do { Checks++; if (!(Cond)) { std::printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, #Cond, qPrintable(Msg)); std::fflush(stdout); Failures++; } } while (0)

static bool WaitFor(const std::function<bool()> & Condition, int Ms = 10000)
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

class TestUserInterface : public UserInterface
{
public:
  std::atomic<int> QueryCalls{0};
  unsigned int QueryAnswer = qaCancel;

  HostKeyAnswer VerifyHostKey(const QString &, int, const QString &, const QString &, const QString &, HostKeyState) override
  {
    return HostKeyCancel;
  }
  bool PromptUser(PromptKind, const QString &, const QString &, QList<Prompt> &) override { return false; }
  void DisplayBanner(const QString &, const QString &) override {}
  unsigned int QueryUser(const QString &, const QString &, unsigned int) override
  {
    QueryCalls++;
    return QueryAnswer;
  }
};

static QStringList Captions(const QList<QAction *> & Actions)
{
  QStringList Result;
  for (QAction * Action : Actions)
  {
    Result.append(Action->isSeparator() ? QStringLiteral("-") : Action->text());
  }
  return Result;
}

// Configuration\Interface\QueueView value (unmunged)
static QString QueueViewValue(const QString & Key)
{
  IniStorage Storage(&Configuration::Instance().Ini());
  if (Storage.OpenSubKey(QStringLiteral("Configuration"), false) && Storage.OpenSubKey(QStringLiteral("Interface"), false) &&
      Storage.OpenSubKey(QStringLiteral("QueueView"), false))
  {
    return Storage.ReadString(Key, QString());
  }
  return QString();
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

  SessionData Data;
  Data.HostName = QStringLiteral("127.0.0.1");
  Data.PortNumber = 1;   // nothing listens: connections fail at once
  Data.UserName = QStringLiteral("nobody");

  TestUserInterface UI;
  QMainWindow Window;
  auto * Panel = new QueuePanel();
  Window.setCentralWidget(Panel);
  Window.resize(900, 300);
  Window.show();
  QTreeWidget * View = Panel->findChild<QTreeWidget *>(QStringLiteral("QueueView3"));
  QLabel * Label = Panel->findChild<QLabel *>(QStringLiteral("QueueLabel"));
  QToolBar * Toolbar = Panel->findChild<QToolBar *>();
  CHECK((View != nullptr) && (Label != nullptr) && (Toolbar != nullptr));
  if ((View == nullptr) || (Label == nullptr) || (Toolbar == nullptr))
  {
    return 1;
  }
  using QA = QueuePanel::QueueAction;

  std::printf("[columns, label, toolbar]\n");
  {
    QStringList Headers;
    for (int Col = 0; Col < View->columnCount(); Col++)
    {
      Headers.append(View->headerItem()->text(Col));
    }
    CHECK_MSG(Headers == (QStringList{QStringLiteral("Operation"), QStringLiteral("Source"), QStringLiteral("Destination"),
      QStringLiteral("Transferred"), QStringLiteral("Time"), QStringLiteral("Speed"), QStringLiteral("Progress")}), Headers.join(QLatin1Char(',')));
    CHECK(View->columnWidth(0) == 70);
    CHECK(Label->text() == QStringLiteral("Queue"));
    // QueueToolbar: Enable | Query Error Prompt Execute Pause Resume Delete | Up Down | DeleteAllDone | OnceEmpty Customize
    QStringList Tool = Captions(Toolbar->actions());
    QStringList Expected{QStringLiteral("&Process Queue"), QStringLiteral("-"), QStringLiteral("&Show Query"), QStringLiteral("&Show Error"),
      QStringLiteral("&Show Prompt"), QStringLiteral("&Execute Now"), QStringLiteral("&Suspend"), QStringLiteral("&Resume"), QStringLiteral("&Cancel"),
      QStringLiteral("-"), QStringLiteral("Move &Up"), QStringLiteral("Move &Down"), QStringLiteral("-"), QStringLiteral("Delete All &Completed"),
      QStringLiteral("-"), QStringLiteral("Once &Empty"), QStringLiteral("&Customize...")};
    CHECK_MSG(Tool == Expected, Tool.join(QLatin1Char('|')));
    CHECK(Panel->Action(QA::Enable)->shortcut() == QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Q));
    CHECK(Panel->Action(QA::GoTo)->shortcut() == QKeySequence(Qt::CTRL | Qt::Key_Q));
    CHECK(!Panel->Action(QA::Enable)->isEnabled());   // no session
    CHECK(qobject_cast<QWidgetAction *>(Panel->Action(QA::ItemSpeed)) != nullptr);
    // window-wide shortcuts, also when the queue is hidden
    CHECK(Window.actions().contains(Panel->Action(QA::Enable)));
    CHECK(Window.actions().contains(Panel->Action(QA::GoTo)));
  }

  std::printf("[pending items: rows, actions, keys]\n");
  {
    TransferQueue Queue(Data, &UI);
    Queue.SetEnabled(false);
    Panel->SetQueue(&Queue);
    CopyParam Param;
    int A = Queue.AddUpload({QStringLiteral("/tmp/a.bin")}, QStringLiteral("/remote"), Param, false);
    int B = Queue.AddDownload({QStringLiteral("/remote/dir/x"), QStringLiteral("/remote/dir/y")}, QStringLiteral("/local"), Param, true);
    int C = Queue.AddDelete({QStringLiteral("/remote/old.txt")}, true);
    QApplication::processEvents();
    CHECK(View->topLevelItemCount() == 3);
    CHECK_MSG(Label->text() == QStringLiteral("Queue (3)"), Label->text());
    QTreeWidgetItem * Row = View->topLevelItem(1);
    CHECK(Row->text(0).isEmpty());                         // images only
    CHECK_MSG(Row->text(1) == QStringLiteral("/remote/dir"), Row->text(1));
    CHECK(Row->text(2) == QStringLiteral("/local"));
    CHECK(Row->text(6) == QStringLiteral("Waiting..."));
    CHECK(Row->toolTip(0) == QStringLiteral("Download and Delete"));
    CHECK(View->topLevelItem(2)->text(1) == QStringLiteral("/remote/old.txt"));
    CHECK(View->topLevelItem(2)->toolTip(0) == QStringLiteral("Delete"));
    CHECK(Panel->Action(QA::Enable)->isEnabled());
    CHECK(!Panel->Action(QA::Enable)->isChecked());

    // focus the second item: Execute (visible), Up, Down; no Pause/Resume/Query
    View->setCurrentItem(View->topLevelItem(1));
    View->topLevelItem(1)->setSelected(true);
    QApplication::processEvents();
    CHECK(Panel->Action(QA::ItemExecute)->isEnabled() && Panel->Action(QA::ItemExecute)->isVisible());
    CHECK(!Panel->Action(QA::ItemPause)->isVisible());
    CHECK(!Panel->Action(QA::ItemResume)->isVisible());
    CHECK(!Panel->Action(QA::ItemQuery)->isVisible());
    CHECK(Panel->Action(QA::ItemUp)->isEnabled());
    CHECK(Panel->Action(QA::ItemDown)->isEnabled());
    CHECK(Panel->Action(QA::ItemDelete)->isEnabled());
    CHECK(Panel->Action(QA::ItemSpeed)->isEnabled());
    CHECK(Panel->Action(QA::DeleteAll)->isEnabled());
    CHECK(!Panel->Action(QA::DeleteAllDone)->isEnabled());
    CHECK(Panel->Action(QA::CycleOnceEmpty)->isEnabled());
    Panel->Action(QA::ItemUp)->trigger();
    QApplication::processEvents();
    QList<QueueItemInfo> Items = Queue.Items();
    CHECK((Items.size() == 3) && (Items[0].Id == B) && (Items[1].Id == A));
    CHECK(View->topLevelItem(0)->data(0, Qt::UserRole).toInt() == B);

    // Delete key on the list cancels the focused item (before the window's Delete shortcut)
    View->setFocus();
    QApplication::processEvents();
    QKeyEvent Press(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QApplication::sendEvent(View, &Press);
    QApplication::processEvents();
    CHECK(Queue.Items().size() == 2);
    CHECK_MSG(Label->text() == QStringLiteral("Queue (2)"), Label->text());

    // Once Empty: cycle Idle -> Disconnect -> Sleep -> Shut Down -> Idle
    CHECK(Panel->OnceEmpty() == QueuePanel::odoIdle);
    Panel->Action(QA::CycleOnceEmpty)->trigger();
    CHECK(Panel->OnceEmpty() == QueuePanel::odoDisconnect);
    CHECK(Panel->Action(QA::DisconnectOnceEmpty)->isChecked());
    Panel->Action(QA::CycleOnceEmpty)->trigger();
    Panel->Action(QA::CycleOnceEmpty)->trigger();
    CHECK(Panel->OnceEmpty() == QueuePanel::odoShutDown);
    Panel->Action(QA::CycleOnceEmpty)->trigger();
    CHECK(Panel->OnceEmpty() == QueuePanel::odoIdle);

    // the queue empties with "Disconnect Session" selected: signal, reset to Stay Idle
    int Requested = -1;
    QObject::connect(Panel, &QueuePanel::OnceEmptyRequested, Panel, [&Requested](int Operation) { Requested = Operation; });
    Panel->Action(QA::DisconnectOnceEmpty)->trigger();
    CHECK(Panel->OnceEmpty() == QueuePanel::odoDisconnect);
    Panel->Action(QA::DeleteAll)->trigger();
    QApplication::processEvents();
    CHECK(Queue.IsEmpty());
    CHECK(Requested == QueuePanel::odoDisconnect);
    CHECK(Panel->OnceEmpty() == QueuePanel::odoIdle);
    CHECK(Label->text() == QStringLiteral("Queue"));
    (void)C;

    // context menu (QueuePopup) structure
    QMenu * Popup = nullptr;
    for (QMenu * Menu : Panel->findChildren<QMenu *>())
    {
      if (Captions(Menu->actions()).contains(QStringLiteral("&Options")))
      {
        Popup = Menu;
      }
    }
    CHECK(Popup != nullptr);
    if (Popup != nullptr)
    {
      QStringList Items2 = Captions(Popup->actions());
      QStringList Expected{QStringLiteral("&Show Query"), QStringLiteral("&Show Error"), QStringLiteral("&Show Prompt"), QStringLiteral("-"),
        QStringLiteral("&Execute Now"), QStringLiteral("&Suspend"), QStringLiteral("&Resume"), QStringLiteral("&Cancel"), QStringLiteral("&Speed"),
        QStringLiteral("-"), QStringLiteral("Move &Up"), QStringLiteral("Move &Down"), QStringLiteral("-"), QStringLiteral("&Process Queue"),
        QStringLiteral("&All"), QStringLiteral("Once &Empty"), QStringLiteral("&Options")};
      CHECK_MSG(Items2 == Expected, Items2.join(QLatin1Char('|')));
      QMenu * Options = Popup->actions().last()->menu();
      CHECK((Options != nullptr) && (Captions(Options->actions()) == (QStringList{QStringLiteral("&Show"), QStringLiteral("Hide when &Empty"),
        QStringLiteral("&Hide"), QStringLiteral("-"), QStringLiteral("&Toolbar"), QStringLiteral("&File List"), QStringLiteral("-"),
        QStringLiteral("&Reset Columns Layout"), QStringLiteral("-"), QStringLiteral("&Customize...")})));
    }
    QMenu Commands;
    Panel->PopulateQueueMenu(&Commands);
    QStringList CommandItems = Captions(Commands.actions());
    CHECK_MSG(CommandItems.first() == QStringLiteral("&Process Queue") && (CommandItems[1] == QStringLiteral("&Go To")) &&
      (CommandItems.last() == QStringLiteral("&All")), CommandItems.join(QLatin1Char('|')));
    Panel->SetQueue(nullptr);
  }

  std::printf("[visibility modes, layout persistence]\n");
  {
    TransferQueue Queue(Data, &UI);
    Queue.SetEnabled(false);
    Panel->SetQueue(&Queue);
    Configuration::Instance().QueueViewShow = qvHideWhenEmpty;
    CHECK(!Panel->ShouldBeVisible());
    Queue.AddUpload({QStringLiteral("/tmp/a.bin")}, QStringLiteral("/remote"), CopyParam(), false);
    CHECK(Panel->ShouldBeVisible());
    bool HideRequested = false;
    QObject::connect(Panel, &QueuePanel::HideRequested, Panel, [&HideRequested]() { HideRequested = true; });
    Panel->Action(QA::Hide)->trigger();
    CHECK(Configuration::Instance().QueueViewShow == qvHide);
    CHECK(HideRequested);
    CHECK(!Panel->ShouldBeVisible());
    CHECK(QueueViewValue(QStringLiteral("LastHideShow")) == QStringLiteral("1"));
    Panel->ToggleVisibility();   // Hide -> Show
    CHECK(Configuration::Instance().QueueViewShow == qvShow);
    CHECK(Panel->ShouldBeVisible());
    CHECK(Panel->property("ForceShow").toBool());
    Panel->ToggleVisibility();   // Show, not empty -> Hide
    CHECK(Configuration::Instance().QueueViewShow == qvHide);
    Panel->Action(QA::HideWhenEmpty)->trigger();
    CHECK(Configuration::Instance().QueueViewShow == qvHideWhenEmpty);
    Panel->setVisible(true);

    View->setColumnWidth(1, 222);
    Spin(500);
    QString Layout = QueueViewValue(QStringLiteral("Layout"));
    CHECK_MSG(Layout.startsWith(QStringLiteral("70,222,")) && Layout.endsWith(QStringLiteral(",;96")), Layout);
    Panel->Action(QA::ResetLayoutColumns)->trigger();
    CHECK(View->columnWidth(1) != 222);
    Panel->Action(QA::Toolbar)->trigger();
    CHECK(!Toolbar->isVisibleTo(Panel));
    CHECK(QueueViewValue(QStringLiteral("ToolBar")) == QStringLiteral("0"));
    Panel->Action(QA::Toolbar)->trigger();
    CHECK(Toolbar->isVisibleTo(Panel));
    Panel->Action(QA::FileList)->trigger();
    CHECK(Panel->Action(QA::FileList)->isChecked());
    Panel->Action(QA::FileList)->trigger();
    Panel->Action(QA::GoTo)->trigger();
    CHECK(View->hasFocus() || !Window.isActiveWindow());
    Panel->SetQueue(nullptr);
  }

  std::printf("[failed connection: Error state, auto popup]\n");
  {
    TransferQueue Queue(Data, &UI);
    Panel->SetQueue(&Queue);
    Configuration::Instance().QueueAutoPopup = false;
    int Id = Queue.AddUpload({QStringLiteral("/tmp/a.bin")}, QStringLiteral("/remote"), CopyParam(), false);
    CHECK(WaitFor([&]() { return (Queue.Items().size() == 1) && Queue.Items().first().Finished; }, 20000));
    QApplication::processEvents();
    CHECK(View->topLevelItem(0)->text(6) == QStringLiteral("Error"));
    CHECK(!View->topLevelItem(0)->toolTip(0).isEmpty());
    View->setCurrentItem(View->topLevelItem(0));
    View->topLevelItem(0)->setSelected(true);
    QApplication::processEvents();
    CHECK(Panel->Action(QA::ItemError)->isVisible() && Panel->Action(QA::ItemError)->isEnabled());
    QString Shot = qEnvironmentVariable("NIXSCP_TEST_SCREENSHOT");
    if (!Shot.isEmpty())
    {
      Queue.SetEnabled(false);
      Queue.AddDownload({QStringLiteral("/remote/dir/x"), QStringLiteral("/remote/dir/y")}, QStringLiteral("/local"), CopyParam(), false);
      QApplication::processEvents();
      Window.grab().save(Shot);
      Queue.SetEnabled(true);
    }
    // QueueAutoPopup: the error pops up once idle; Cancel removes the item
    UI.QueryAnswer = qaCancel;
    int Calls = UI.QueryCalls;
    Configuration::Instance().QueueAutoPopup = true;
    CHECK(WaitFor([&]() { return UI.QueryCalls > Calls; }, 5000));
    CHECK(WaitFor([&]() { return Queue.UserActionItem() != Id; }, 5000));
    Panel->SetQueue(nullptr);
  }

  std::printf("test_queuepanel: %d checks, %d failure(s)\n", Checks, Failures);
  return (Failures == 0) ? 0 : 1;
}

#endif
