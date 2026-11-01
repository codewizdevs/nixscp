// Offscreen GUI test of the synchronization windows (gui/SynchronizeDialogs,
// WinSCP forms/FullSynchronize, forms/SynchronizeChecklist, forms/Synchronize):
// directory history, defaults, split buttons, checklist Calculate / Find Move
// Candidate / Move / synchronization inside the checklist, WindowParams,
// keep up to date with the MaxWatchDirectories prompt. NIXSCP_GUI_TESTS.
//
// The parts with a session need NIXSCP_TEST_SSH_PORT (+ _KEY, _USER, _HOST);
// the server must run on this machine as the same user.
// NIXSCP_SCREENSHOT_DIR=<dir> keeps the screenshots.
#ifndef NIXSCP_GUI_TESTS

#include <cstdio>

int main()
{
  std::printf("SKIP: test_syncdialogs needs NIXSCP_GUI_TESTS (Qt Widgets + the GUI sources)\n");
  return 0;
}

#else

#include "core/Configuration.h"
#include "core/IniStorage.h"
#include "gui/GuiUserInterface.h"
#include "gui/EditMaskDialog.h"
#include "gui/SynchronizeDialogs.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>

#include <cstdio>
#include <fcntl.h>
#include <functional>
#include <sys/stat.h>

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_MSG(Cond, Msg) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, #Cond, qPrintable(Msg)); std::fflush(stdout); Failures++; } } while (0)

static QString ScreenshotDir;

static void Screenshot(QWidget * Widget, const QString & Name)
{
  QString Path = QDir(ScreenshotDir).filePath(Name);
  CHECK(Widget->grab().save(Path));
  std::printf("  screenshot %s\n", qPrintable(Path));
}

static bool WaitUntil(const std::function<bool()> & Condition, int TimeoutMs)
{
  QElapsedTimer Timer;
  Timer.start();
  while (!Condition())
  {
    if (Timer.elapsed() > TimeoutMs)
    {
      return false;
    }
    QEventLoop Loop;
    QTimer::singleShot(20, &Loop, &QEventLoop::quit);
    Loop.exec();
  }
  return true;
}

static QString MessageText(QWidget * W)
{
  if (auto * Form = qobject_cast<MessageForm *>(W))
  {
    return Form->FormText();
  }
  auto * Box = qobject_cast<QMessageBox *>(W);
  return (Box != nullptr) ? (Box->text() + QLatin1Char('\n') + Box->informativeText()) : QString();
}

// Runs the handlers on modal windows as they appear (each window once)
class ModalScript
{
public:
  using Handler = std::function<bool(QWidget *)>;   // true = handled

  ModalScript()
  {
    FTimer.setInterval(30);
    QObject::connect(&FTimer, &QTimer::timeout, [this]() { Tick(); });
    FTimer.start();
  }
  void Add(Handler H) { FHandlers.push_back(std::move(H)); }
  QStringList Seen;

private:
  QTimer FTimer;
  std::vector<Handler> FHandlers;

  void Tick()
  {
    QWidget * W = QApplication::activeModalWidget();
    if (qEnvironmentVariableIsSet("NIXSCP_TEST_DEBUG") && (W != nullptr))
    {
      std::printf("  modal: %s | %s\n", qPrintable(W->windowTitle()), qPrintable(MessageText(W).left(60)));
      std::fflush(stdout);
    }
    // (handled windows are marked: a new window may get the address of a closed one)
    if ((W == nullptr) || W->property("TestHandled").toBool() || !W->isVisible())
    {
      return;
    }
    for (Handler & H : FHandlers)
    {
      if (H(W))
      {
        W->setProperty("TestHandled", true);
        Seen.append(W->windowTitle());
        return;
      }
    }
  }
};

template <class T>
static T * ByText(QWidget * Parent, const QString & Text)
{
  for (T * W : Parent->findChildren<T *>())
  {
    if (W->text() == Text)
    {
      return W;
    }
  }
  return nullptr;
}

static QAction * ActionByText(QWidget * Parent, const QString & Text)
{
  for (QAction * A : Parent->findChildren<QAction *>())
  {
    if (A->text() == Text)
    {
      return A;
    }
  }
  return nullptr;
}


static void WriteFile(const QString & FileName, const QByteArray & Data, qint64 MTime)
{
  QDir().mkpath(QFileInfo(FileName).path());
  QFile F(FileName);
  CHECK(F.open(QIODevice::WriteOnly | QIODevice::Truncate));
  F.write(Data);
  F.close();
  struct timespec Times[2] = {{static_cast<time_t>(MTime), 0}, {static_cast<time_t>(MTime), 0}};
  utimensat(AT_FDCWD, QFile::encodeName(FileName).constData(), Times, 0);
}

static QByteArray ReadAll(const QString & FileName)
{
  QFile F(FileName);
  return F.open(QIODevice::ReadOnly) ? F.readAll() : QByteArray("<missing>");
}

static const qint64 T0 = QDateTime::currentSecsSinceEpoch() - 100000;

static void TestFullSynchronizeDialog(QWidget * Main)
{
  std::printf("[synchronize dialog]\n");
  EditMaskDialog::SaveToHistory(QStringLiteral("LocalDirectory"), QStringLiteral("/history/local"));
  EditMaskDialog::SaveToHistory(QStringLiteral("RemoteDirectory"), QStringLiteral("/history/remote"));
  {
    ModalScript Script;
    bool Checked = false;
    Script.Add([&](QWidget * W)
    {
      if (W->windowTitle() != QStringLiteral("Synchronize"))
      {
        return false;
      }
      QList<QComboBox *> Combos = W->findChildren<QComboBox *>();
      CHECK(Combos.size() == 2);
      for (QComboBox * Combo : Combos)
      {
        CHECK_MSG(Combo->count() >= 1, Combo->currentText());
      }
      CHECK(Combos.value(0)->currentText() == QStringLiteral("/home/local"));
      CHECK((Combos.value(0)->findText(QStringLiteral("/history/local")) >= 0) || (Combos.value(1)->findText(QStringLiteral("/history/local")) >= 0));
      // Linux: case sensitive by default
      QCheckBox * CaseSensitive = ByText<QCheckBox>(W, QStringLiteral("C&ase sensitive"));
      CHECK((CaseSensitive != nullptr) && CaseSensitive->isChecked());
      QCheckBox * Preview = ByText<QCheckBox>(W, QStringLiteral("Pre&view changes"));
      CHECK((Preview != nullptr) && Preview->isChecked());
      // split buttons: Transfer settings always has the presets drop-down,
      // OK only with Start in New Window
      int VisibleArrows = 0;
      for (QToolButton * Arrow : W->findChildren<QToolButton *>())
      {
        if (Arrow->arrowType() == Qt::DownArrow && Arrow->isVisible())
        {
          VisibleArrows++;
        }
      }
      CHECK_MSG(VisibleArrows == 1, QString::number(VisibleArrows));
      Screenshot(W, QStringLiteral("synchronize.png"));
      Combos.value(0)->setEditText(QStringLiteral("/typed/local"));
      ByText<QPushButton>(W, QStringLiteral("OK"))->click();
      Checked = true;
      return true;
    });
    bool Finished = false;
    RunFullSynchronize(Main, nullptr, QStringLiteral("/home/local"), QStringLiteral("/home/remote"), true, CopyParam(), {},
      [&]() { Finished = true; });
    CHECK(Checked);
    CHECK(Finished);
    // THistoryComboBox::SaveToHistory on OK
    CHECK_MSG(EditMaskDialog::History(QStringLiteral("LocalDirectory")).value(0) == QStringLiteral("/typed/local"),
      EditMaskDialog::History(QStringLiteral("LocalDirectory")).join(QLatin1Char(',')));
    CHECK(EditMaskDialog::History(QStringLiteral("RemoteDirectory")).value(0) == QStringLiteral("/home/remote"));
  }
  {
    // Start in New Window offered with the hook
    bool Called = false;
    SynchronizeHooks().FullSynchronizeInNewWindow = [&](Terminal *, SynchronizeMode, int, const QString &, const QString &, const CopyParam &)
    {
      Called = true;
    };
    ModalScript Script;
    int VisibleArrows = 0;
    Script.Add([&](QWidget * W)
    {
      for (QToolButton * Arrow : W->findChildren<QToolButton *>())
      {
        if (Arrow->arrowType() == Qt::DownArrow && Arrow->isVisible())
        {
          VisibleArrows++;
        }
      }
      ByText<QPushButton>(W, QStringLiteral("Cancel"))->click();
      return true;
    });
    RunFullSynchronize(Main, nullptr, QStringLiteral("/a"), QStringLiteral("/b"), true, CopyParam(), {}, nullptr);
    CHECK_MSG(VisibleArrows == 2, QString::number(VisibleArrows));
    CHECK(!Called);
    SynchronizeHooks().FullSynchronizeInNewWindow = nullptr;
  }
}

static void TestChecklist(QWidget * Main, Terminal * Term, const QString & Local, const QString & Remote)
{
  std::printf("[checklist]\n");
  QDir(Local).removeRecursively();
  QDir(Remote).removeRecursively();
  QDir().mkpath(Local);
  QDir().mkpath(Remote);
  // a file renamed locally (move candidate: same size, same directory)
  WriteFile(QDir(Local).filePath(QStringLiteral("renamed.dat")), QByteArray(777, 'r'), T0);
  WriteFile(QDir(Remote).filePath(QStringLiteral("original.dat")), QByteArray(777, 'r'), T0);
  // a new local directory (Calculate)
  WriteFile(QDir(Local).filePath(QStringLiteral("newdir/a.bin")), QByteArray(1000, 'a'), T0);
  WriteFile(QDir(Local).filePath(QStringLiteral("newdir/b.bin")), QByteArray(24, 'b'), T0);
  // an update
  WriteFile(QDir(Local).filePath(QStringLiteral("update.txt")), "new content", T0 + 100);
  WriteFile(QDir(Remote).filePath(QStringLiteral("update.txt")), "old content", T0);

  ModalScript Script;
  bool ChecklistDone = false;
  bool SummaryShown = false;
  QString ChecklistTitleWhileSynchronizing;
  Script.Add([&](QWidget * W)
  {
    if (W->windowTitle() != QStringLiteral("Synchronize"))
    {
      return false;
    }
    // Remote direction with Delete, so the remote original.dat is "obsolete"
    ByText<QAbstractButton>(W, QStringLiteral("&Remote"))->click();
    QCheckBox * Delete = ByText<QCheckBox>(W, QStringLiteral("&Delete files"));
    Delete->setChecked(true);
    ByText<QCheckBox>(W, QStringLiteral("Pre&view changes"))->setChecked(true);
    ByText<QPushButton>(W, QStringLiteral("OK"))->click();
    return true;
  });
  Script.Add([&](QWidget * W)
  {
    if (W->windowTitle() != QStringLiteral("Synchronization checklist"))
    {
      return false;
    }
    auto * Dialog = qobject_cast<QDialog *>(W);
    QTreeWidget * List = W->findChild<QTreeWidget *>();
    CHECK(List != nullptr);
    // renamed.dat (upload new), original.dat (delete remote), newdir/ (upload new), update.txt (upload update)
    CHECK_MSG(List->topLevelItemCount() == 4, QString::number(List->topLevelItemCount()));
    auto ItemByName = [List](const QString & Name) -> QTreeWidgetItem *
    {
      for (int Index = 0; Index < List->topLevelItemCount(); Index++)
      {
        if (List->topLevelItem(Index)->text(0) == Name)
        {
          return List->topLevelItem(Index);
        }
      }
      return nullptr;
    };
    QTreeWidgetItem * NewDir = ItemByName(QStringLiteral("newdir/"));
    CHECK(NewDir != nullptr);
    QAction * Calculate = ActionByText(W, QStringLiteral("C&alculate"));
    QAction * CalculateAll = ActionByText(W, QStringLiteral("Calc&ulate All"));
    QAction * FindMove = ActionByText(W, QStringLiteral("&Find Move Candidate"));
    QAction * Move = ActionByText(W, QStringLiteral("&Move"));
    CHECK((Calculate != nullptr) && (CalculateAll != nullptr) && (FindMove != nullptr) && (Move != nullptr));
    CHECK(Move->shortcut() == QKeySequence(Qt::Key_F6));
    CHECK(ActionByText(W, QStringLiteral("Copy &Path to Clipboard"))->shortcut() == QKeySequence(Qt::CTRL | Qt::Key_BracketLeft));
    if (NewDir != nullptr)
    {
      CHECK(NewDir->text(2).isEmpty());
      // Calculate All
      CalculateAll->trigger();
      CHECK(WaitUntil([&]() { return !NewDir->text(2).isEmpty(); }, 20000));
      std::printf("  newdir size: %s\n", qPrintable(NewDir->text(2)));
      CHECK(NewDir->data(2, Qt::UserRole + 1).toLongLong() == 1024);
    }
    // Find Move Candidate from the renamed file selects the pair
    QTreeWidgetItem * Renamed = ItemByName(QStringLiteral("renamed.dat"));
    QTreeWidgetItem * Original = ItemByName(QStringLiteral("original.dat"));
    CHECK((Renamed != nullptr) && (Original != nullptr));
    if ((Renamed != nullptr) && (Original != nullptr))
    {
      List->clearSelection();
      List->setCurrentItem(Renamed, 0, QItemSelectionModel::NoUpdate);
      FindMove->trigger();
      CHECK(Renamed->isSelected() && Original->isSelected());
      CHECK(List->selectedItems().size() == 2);
      CHECK(Move->isEnabled());
      Screenshot(W, QStringLiteral("checklist.png"));
      // Move: the remote file is renamed, both items disappear
      Move->trigger();
      CHECK(WaitUntil([&]() { return List->topLevelItemCount() == 2; }, 20000));
      CHECK(ReadAll(QDir(Remote).filePath(QStringLiteral("renamed.dat"))) == QByteArray(777, 'r'));
      CHECK(!QFileInfo::exists(QDir(Remote).filePath(QStringLiteral("original.dat"))));
    }
    // OK: synchronizes inside the checklist
    QPushButton * Ok = ByText<QPushButton>(W, QStringLiteral("OK"));
    CHECK((Ok != nullptr) && Ok->isEnabled());
    Ok->click();
    ChecklistTitleWhileSynchronizing = W->windowTitle();
    CHECK(WaitUntil([&]() { return !Dialog->isVisible(); }, 30000));
    ChecklistDone = true;
    return true;
  });
  Script.Add([&](QWidget * W)
  {
    if (!MessageText(W).contains(QStringLiteral("Synchronization was completed.")))
    {
      return false;
    }
    SummaryShown = true;
    Screenshot(W, QStringLiteral("summary.png"));
    qobject_cast<MessageForm *>(W)->SubmitAnswer(qaOK);
    return true;
  });
  bool Finished = false;
  RunFullSynchronize(Main, Term, Local, Remote, true, CopyParam(), {}, [&]() { Finished = true; });
  CHECK(WaitUntil([&]() { return Finished; }, 60000));
  CHECK(ChecklistDone);
  CHECK(SummaryShown);
  CHECK_MSG(ChecklistTitleWhileSynchronizing == QStringLiteral("Synchronizing"), ChecklistTitleWhileSynchronizing);
  CHECK(ReadAll(QDir(Remote).filePath(QStringLiteral("update.txt"))) == "new content");
  CHECK(ReadAll(QDir(Remote).filePath(QStringLiteral("newdir/a.bin"))).size() == 1000);
  // SynchronizeChecklist\WindowParams / ListParams saved
  SynchronizeConfiguration Config = SynchronizeConfiguration::Load();
  CHECK_MSG(Config.ChecklistWindowParams.split(QLatin1Char(';')).size() == 7, Config.ChecklistWindowParams);
  CHECK_MSG(Config.ChecklistListParams.count(QLatin1Char('|')) == 2, Config.ChecklistListParams);
  IniStorage Storage(&Configuration::Instance().Ini());
  CHECK(Storage.OpenSubKeyPath(QStringLiteral("Configuration\\Interface\\SynchronizeChecklist"), false));
  CHECK(!Storage.ReadString(QStringLiteral("WindowParams"), QString()).isEmpty());
}

static void TestKeepUpToDate(QWidget * Main, Terminal * Term, const QString & Local, const QString & Remote)
{
  std::printf("[keep remote directory up to date]\n");
  QDir(Local).removeRecursively();
  for (int Index = 0; Index < 4; Index++)
  {
    QDir().mkpath(QDir(Local).filePath(QStringLiteral("d%1").arg(Index)));
  }
  QDir().mkpath(Remote);
  SynchronizeConfiguration Config = SynchronizeConfiguration::Load();
  Config.MaxWatchDirectories = 2;
  Config.Save();

  ModalScript Script;
  bool Asked = false;
  bool Stopped = false;
  QString TooManyText;
  QDialog * Dialog = nullptr;
  Script.Add([&](QWidget * W)
  {
    if (W->windowTitle() != QStringLiteral("Keep remote directory up to date"))
    {
      return false;
    }
    Dialog = qobject_cast<QDialog *>(W);
    QPushButton * Close = ByText<QPushButton>(W, QStringLiteral("Close"));
    QPushButton * Minimize = ByText<QPushButton>(W, QStringLiteral("&Minimize"));
    CHECK((Close != nullptr) && Close->isVisible());
    CHECK((Minimize != nullptr) && !Minimize->isVisible());
    Screenshot(W, QStringLiteral("keepuptodate.png"));
    QCheckBox * OnStart = ByText<QCheckBox>(W, QStringLiteral("Synchronize on s&tart"));
    OnStart->setCheckState(Qt::Unchecked);
    ByText<QPushButton>(W, QStringLiteral("&Start"))->click();
    return true;
  });
  Script.Add([&](QWidget * W)
  {
    QString Text = MessageText(W);
    if (!Text.contains(QStringLiteral("directories and subdirectories found")))
    {
      return false;
    }
    TooManyText = Text;
    Asked = true;
    // Yes + Never ask me again: the limit doubles to 4 and is saved, the
    // fifth directory asks again
    auto * Form = qobject_cast<MessageForm *>(W);
    if (TooManyText.contains(QStringLiteral("More than 2 ")))
    {
      Form->NeverAskAgainCheck()->setChecked(true);
      Form->SubmitAnswer(qaYes);
    }
    else
    {
      Form->SubmitAnswer(qaNo);
      Stopped = true;
    }
    return true;
  });
  Script.Add([&](QWidget * W)
  {
    if (!MessageText(W).contains(QStringLiteral("More than 4 ")))
    {
      return false;
    }
    qobject_cast<MessageForm *>(W)->SubmitAnswer(qaNo);
    Stopped = true;
    return true;
  });
  QTimer::singleShot(0, [&]()
  {
    // closes the dialog once the watching was aborted
    WaitUntil([&]() { return Stopped && (Dialog != nullptr); }, 20000);
    WaitUntil([&]() { QPushButton * Close = ByText<QPushButton>(Dialog, QStringLiteral("Close")); return Close && Close->isVisible(); }, 5000);
    Screenshot(Dialog, QStringLiteral("keepuptodate-stopped.png"));
    Dialog->reject();
  });
  bool Finished = false;
  RunKeepRemoteUpToDate(Main, Term, Local, Remote, CopyParam(), {}, [&]() { Finished = true; });
  CHECK(Finished);
  CHECK(Asked);
  CHECK(Stopped);
  CHECK_MSG(SynchronizeConfiguration::Load().MaxWatchDirectories == 4, QString::number(SynchronizeConfiguration::Load().MaxWatchDirectories));
}

class TestUserInterface : public UserInterface
{
public:
  HostKeyAnswer VerifyHostKey(const QString &, int, const QString &, const QString &, const QString &, HostKeyState) override
  {
    return HostKeyOnce;
  }
  bool PromptUser(PromptKind, const QString &, const QString &, QList<Prompt> &) override { return false; }
  void DisplayBanner(const QString &, const QString &) override {}
  unsigned int QueryUser(const QString &, const QString &, unsigned int) override { return qaCancel; }
};

int main(int argc, char ** argv)
{
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
  {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  QApplication App(argc, argv);
  QStandardPaths::setTestModeEnabled(true);
  QTemporaryDir Scratch;
  qputenv("XDG_CONFIG_HOME", Scratch.path().toUtf8());
  qputenv("XDG_DATA_HOME", Scratch.filePath(QStringLiteral("data")).toUtf8());
  Configuration::Instance().SetIniFileName(Scratch.filePath(QStringLiteral("nixscp.ini")));
  Configuration::Instance().Load();
  ScreenshotDir = qEnvironmentVariable("NIXSCP_SCREENSHOT_DIR", Scratch.path());
  QDir().mkpath(ScreenshotDir);

  QMainWindow Main;
  Main.resize(1000, 700);
  Main.show();

  TestFullSynchronizeDialog(&Main);

  QByteArray PortStr = qgetenv("NIXSCP_TEST_SSH_PORT");
  if (!PortStr.isEmpty())
  {
    SessionData Data;
    Data.HostName = qEnvironmentVariable("NIXSCP_TEST_SSH_HOST", QStringLiteral("127.0.0.1"));
    Data.PortNumber = PortStr.toInt();
    Data.UserName = qEnvironmentVariable("NIXSCP_TEST_SSH_USER", qEnvironmentVariable("USER"));
    Data.PublicKeyFile = qEnvironmentVariable("NIXSCP_TEST_SSH_KEY");
    Data.TryAgent = false;
    Data.AuthKI = false;
    Data.KnownHostsFile = Scratch.filePath(QStringLiteral("dotssh/known_hosts"));
    TestUserInterface UI;
    Terminal T(Data, &UI);
    bool Called = false;
    T.Open([&](const OperationResult &) { Called = true; });
    CHECK(WaitUntil([&]() { return Called; }, 30000));
    CHECK(T.Active());
    if (T.Active())
    {
      QString Local = Scratch.filePath(QStringLiteral("local"));
      QString Remote = Scratch.filePath(QStringLiteral("remote"));
      TestChecklist(&Main, &T, Local, Remote);
      TestKeepUpToDate(&Main, &T, Local, Remote);
    }
  }
  else
  {
    std::printf("test_syncdialogs: session tests SKIPPED (set NIXSCP_TEST_SSH_PORT, NIXSCP_TEST_SSH_KEY)\n");
  }

  std::printf(Failures ? "test_syncdialogs: %d FAILURES\n" : "test_syncdialogs: OK\n", Failures);
  return Failures ? 1 : 0;
}

#endif
