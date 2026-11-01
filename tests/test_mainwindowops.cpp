// Main window operation tests (gui/CommanderWindow, FilePanel, FileListModel,
// QueuePanel, ProgressDialog, PreferencesDialog Integration page), ports of
// WinSCP forms/CustomScpExplorer:
// - Calculate Directory Sizes shows the sizes in the panel Size column
//   (SetItemCalculatedSize), also counted by the status bar totals
// - Proceed in Background (TProgressForm MoveToQueue): the files not yet
//   transferred continue in the session's queue
// - Once Finished "Disconnect Session" of the progress dialog and Once Empty
//   "Disconnect Session" of the queue close the session (CloseOnCompletion)
// - qePendingUserAction: the item waiting for the user is selected in the
//   queue list; with QueueAutoPopup its query pops up
// - TransferQueue::ApplyConfiguration after Preferences OK
// - Preferences > Integration > Register to handle URL addresses (desktop
//   entry + xdg-mime), Unregister
//
// GUI test: built with NIXSCP_GUI_TESTS and the GUI objects, runs offscreen.
// The session part needs NIXSCP_TEST_SSH_PORT and NIXSCP_TEST_SSH_KEY
// (NIXSCP_TEST_SSH_HOST, NIXSCP_TEST_SSH_USER optional); the test sshd
// serves the local file system, so remote files are checked locally.
#ifndef NIXSCP_GUI_TESTS

#include <cstdio>

int main()
{
  std::printf("SKIP: test_mainwindowops needs NIXSCP_GUI_TESTS (Qt Widgets + the GUI sources)\n");
  return 0;
}

#else

#include "core/Configuration.h"
#include "core/CopyParamList.h"
#include "core/Queue.h"
#include "gui/CommanderActions.h"
#include "gui/CommanderWindow.h"
#include "gui/CopyDialog.h"
#include "gui/FileListModel.h"
#include "gui/FilePanel.h"
#include "gui/PreferencesDialog.h"
#include "gui/ProgressDialog.h"
#include "gui/QueuePanel.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDialogButtonBox>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QProcess>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QTreeView>
#include <QTreeWidget>

#include <cstdio>
#include <functional>

static int Failures = 0;
static int Checks = 0;

#define CHECK(Cond) \
  do { Checks++; if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_MSG(Cond, Msg) \
  do { Checks++; if (!(Cond)) { std::printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, #Cond, qPrintable(Msg)); std::fflush(stdout); Failures++; } } while (0)

static bool WaitFor(const std::function<bool()> & Condition, int Ms = 20000)
{
  QElapsedTimer Timer;
  Timer.start();
  while (!Condition())
  {
    if (Timer.elapsed() > Ms) return false;
    QApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(5);
  }
  return true;
}

// Answers modal dialogs: CopyDialog OK, message boxes with MessageBoxAnswer,
// anything else is recorded and rejected
static QMessageBox::StandardButton MessageBoxAnswer = QMessageBox::No;
static QStringList MessageBoxTexts;
static QStringList OtherModals;
static int CopyDialogs = 0;

static void InstallModalHandler()
{
  auto * Timer = new QTimer(qApp);
  Timer->setInterval(30);
  QObject::connect(Timer, &QTimer::timeout, []()
  {
    QWidget * Modal = QApplication::activeModalWidget();
    if (Modal == nullptr) return;
    if (auto * Copy = qobject_cast<CopyDialog *>(Modal))
    {
      CopyDialogs++;
      Copy->accept();
    }
    else if (auto * Box = qobject_cast<QMessageBox *>(Modal))
    {
      MessageBoxTexts.append(Box->text());
      QAbstractButton * Button = Box->button(MessageBoxAnswer);
      if (Button == nullptr) Button = Box->button(QMessageBox::Ok);
      if (Button != nullptr) Button->click();
      else Box->reject();
    }
    else if ((qobject_cast<PreferencesDialog *>(Modal) != nullptr) ||
             (QString::fromLatin1(Modal->metaObject()->className()) == QStringLiteral("AuthenticateDialog")))
    {
      // left to the test
    }
    else if (auto * Dialog = qobject_cast<QDialog *>(Modal))
    {
      OtherModals.append(QString::fromLatin1(Dialog->metaObject()->className()) + QLatin1Char(':') + Dialog->windowTitle());
      Dialog->reject();
    }
  });
  Timer->start();
}

static FilePanel * Panel(CommanderWindow & Window, bool Right)
{
  for (QSplitter * Splitter : Window.findChildren<QSplitter *>())
  {
    if (!Splitter->isVisibleTo(&Window) || (Splitter->count() != 2)) continue;
    auto * Left = qobject_cast<FilePanel *>(Splitter->widget(0));
    auto * RightPanel = qobject_cast<FilePanel *>(Splitter->widget(1));
    if ((Left != nullptr) && (RightPanel != nullptr)) return Right ? RightPanel : Left;
  }
  return nullptr;
}

static QString CellText(FilePanel * P, const QString & Name, int Column)
{
  QAbstractItemModel * Model = P->View()->model();
  for (int Row = 0; Row < Model->rowCount(); Row++)
  {
    if (Model->index(Row, FileListModel::ColName).data().toString() == Name)
    {
      return Model->index(Row, Column).data().toString();
    }
  }
  return QStringLiteral("<missing>");
}

static void WriteFile(const QString & FileName, qint64 Size)
{
  QFile F(FileName);
  (void)F.open(QIODevice::WriteOnly);
  F.write(QByteArray(Size, 'x'));
}

static SessionData TestSession(const QString & RemoteDir, const QString & LocalDir)
{
  SessionData Data;
  Data.FSProtocol = fsSFTP;
  Data.HostName = qEnvironmentVariable("NIXSCP_TEST_SSH_HOST", QStringLiteral("127.0.0.1"));
  Data.PortNumber = qEnvironmentVariable("NIXSCP_TEST_SSH_PORT").toInt();
  Data.UserName = qEnvironmentVariable("NIXSCP_TEST_SSH_USER", qEnvironmentVariable("USER"));
  Data.PublicKeyFile = qEnvironmentVariable("NIXSCP_TEST_SSH_KEY");
  Data.HostKey = QStringLiteral("*");
  Data.RemoteDirectory = RemoteDir;
  Data.LocalDirectory = LocalDir;
  return Data;
}

static bool Connect(CommanderWindow & Window, const QString & RemoteDir, const QString & LocalDir)
{
  Window.OpenSession(TestSession(RemoteDir, LocalDir));
  bool Result = WaitFor([&]()
  {
    FilePanel * R = Panel(Window, true);
    return (R != nullptr) && R->IsRemote() && (R->Provider() != nullptr) && !R->IsLoading() &&
      (R->CurrentDirectory() == RemoteDir);
  });
  if (!Result)
  {
    FilePanel * R = Panel(Window, true);
    std::printf("  connect failed: panel %p remote %d provider %p dir '%s'; modals: %s; boxes: %s\n", static_cast<void *>(R),
      (R != nullptr) && R->IsRemote(), (R != nullptr) ? static_cast<void *>(R->Provider()) : nullptr,
      (R != nullptr) ? qPrintable(R->CurrentDirectory()) : "", qPrintable(OtherModals.join(QLatin1Char(','))),
      qPrintable(MessageBoxTexts.join(QLatin1Char(','))));
  }
  return Result;
}

static bool RightConnected(CommanderWindow & Window)
{
  FilePanel * R = Panel(Window, true);
  return (R != nullptr) && (R->Provider() != nullptr);
}

static ProgressDialog * VisibleProgress(CommanderWindow & Window)
{
  for (ProgressDialog * P : Window.findChildren<ProgressDialog *>())
  {
    if (P->isVisible()) return P;
  }
  return nullptr;
}

static QAction * ProgressAction(ProgressDialog * Progress, const QString & Text)
{
  for (QAction * A : Progress->findChildren<QAction *>())
  {
    if (A->text() == Text) return A;
  }
  return nullptr;
}

static void SelectNames(FilePanel * P, const QStringList & Names)
{
  P->FocusView();
  emit P->Activated();
  P->SelectFiles(Names);
  QApplication::processEvents();
}

//---------------------------------------------------------------------------

static void TestDirectorySizes(CommanderWindow & Window, const QString & Root)
{
  std::printf("[directory sizes]\n");
  QString RemoteDir = QDir(Root).filePath(QStringLiteral("sizes-remote"));
  QString LocalDir = QDir(Root).filePath(QStringLiteral("sizes-local"));
  QDir().mkpath(RemoteDir + QStringLiteral("/dir1/sub"));
  QDir().mkpath(LocalDir + QStringLiteral("/ldir"));
  WriteFile(RemoteDir + QStringLiteral("/dir1/a.bin"), 3000);
  WriteFile(RemoteDir + QStringLiteral("/dir1/sub/b.bin"), 2000);
  WriteFile(RemoteDir + QStringLiteral("/top.bin"), 100);
  WriteFile(LocalDir + QStringLiteral("/ldir/c.bin"), 7000);
  CHECK(Connect(Window, RemoteDir, LocalDir));
  FilePanel * R = Panel(Window, true);
  FilePanel * L = Panel(Window, false);
  CHECK(WaitFor([&]() { return L->CurrentDirectory() == LocalDir; }));
  CHECK(CellText(R, QStringLiteral("dir1"), FileListModel::ColSize).isEmpty());

  SelectNames(R, {QStringLiteral("dir1")});
  Window.Actions()->Get(QStringLiteral("CalculateDirectorySizesAction"))->trigger();
  QString Expected = FileListModel::FormatPanelBytes(5000, Configuration::Instance().FormatSizeBytes);
  CHECK_MSG(WaitFor([&]() { return CellText(R, QStringLiteral("dir1"), FileListModel::ColSize) == Expected; }),
    CellText(R, QStringLiteral("dir1"), FileListModel::ColSize));
  std::printf("  remote dir1 size column: '%s'\n", qPrintable(CellText(R, QStringLiteral("dir1"), FileListModel::ColSize)));
  // the selected directory size counts in the status bar ("5,000 B of 5,100 B in 1 of 2")
  bool StatusOk = false;
  for (QLabel * Label : R->findChildren<QLabel *>())
  {
    if (Label->text().contains(FileListModel::FormatBytes(5000)) && Label->text().contains(FileListModel::FormatBytes(5100)))
    {
      StatusOk = true;
    }
  }
  CHECK(StatusOk);
  // sorting by size uses the calculated size
  R->SetSort(FileListModel::ColSize, true);

  SelectNames(L, {QStringLiteral("ldir")});
  Window.Actions()->Get(QStringLiteral("LocalCalculateDirectorySizesAction"))->trigger();
  QString ExpectedLocal = FileListModel::FormatPanelBytes(7000, Configuration::Instance().FormatSizeBytes);
  CHECK_MSG(WaitFor([&]() { return CellText(L, QStringLiteral("ldir"), FileListModel::ColSize) == ExpectedLocal; }),
    CellText(L, QStringLiteral("ldir"), FileListModel::ColSize));
  // a reload of the unchanged directory keeps the size; a change of the entry drops it
  R->Refresh();
  CHECK(WaitFor([&]() { return !R->IsLoading(); }));
  CHECK(CellText(R, QStringLiteral("dir1"), FileListModel::ColSize) == Expected);
  WriteFile(RemoteDir + QStringLiteral("/dir1/new.bin"), 10);
  // a different modification time of the directory entry
  QProcess::execute(QStringLiteral("touch"), {QStringLiteral("-m"), QStringLiteral("-d"), QStringLiteral("2020-01-01 12:00"),
    RemoteDir + QStringLiteral("/dir1")});
  R->Refresh();
  CHECK(WaitFor([&]() { return !R->IsLoading() && CellText(R, QStringLiteral("dir1"), FileListModel::ColSize).isEmpty(); }));
}

static void TestMoveToQueue(CommanderWindow & Window, const QString & Root)
{
  std::printf("[proceed in background]\n");
  QString RemoteDir = QDir(Root).filePath(QStringLiteral("bg-remote"));
  QString LocalDir = QDir(Root).filePath(QStringLiteral("bg-local"));
  QDir().mkpath(RemoteDir);
  QDir().mkpath(LocalDir);
  QStringList Names;
  for (int Index = 1; Index <= 5; Index++)
  {
    QString Name = QStringLiteral("f%1.bin").arg(Index);
    WriteFile(LocalDir + QLatin1Char('/') + Name, 200 * 1024);
    Names.append(Name);
  }
  FilePanel * R = Panel(Window, true);
  FilePanel * L = Panel(Window, false);
  R->ChangeDirectory(RemoteDir);
  L->ChangeDirectory(LocalDir);
  CHECK(WaitFor([&]() { return (R->CurrentDirectory() == RemoteDir) && (L->CurrentDirectory() == LocalDir) && !R->IsLoading(); }));
  // 200 KB/s: about a second per file
  Configuration::Instance().DefaultCopyParam().CPSLimit = 200 * 1024;
  SelectNames(L, Names);
  CopyDialogs = 0;
  Window.Actions()->Get(QStringLiteral("LocalCopyAction"))->trigger();
  ProgressDialog * Progress = nullptr;
  CHECK(WaitFor([&]() { Progress = VisibleProgress(Window); return Progress != nullptr; }));
  CHECK(CopyDialogs == 1);
  if (Progress == nullptr) return;
  QAction * MoveToQueue = ProgressAction(Progress, QStringLiteral("Proceed in &Background"));
  CHECK((MoveToQueue != nullptr) && MoveToQueue->isVisible() && MoveToQueue->isEnabled());
  if (MoveToQueue == nullptr) return;
  QueuePanel * Queue = Window.findChild<QueuePanel *>();
  QList<QueueItemInfo> Items;
  // the transfer stops after the current file; the rest goes to the queue
  CHECK(WaitFor([&]() { QApplication::processEvents(); return QFileInfo::exists(RemoteDir + QStringLiteral("/f1.bin")); }));
  auto Sizes = [&]()
  {
    QStringList Result;
    for (const QString & Name : Names) Result.append(QString::number(QFileInfo(RemoteDir + QLatin1Char('/') + Name).size()));
    return Result.join(QLatin1Char(' '));
  };
  std::printf("  remote sizes at Proceed in Background: %s\n", qPrintable(Sizes()));
  QElapsedTimer Timer;
  Timer.start();
  MoveToQueue->trigger();
  CHECK(WaitFor([&]()
  {
    if ((Queue->Queue() != nullptr) && !Queue->Queue()->Items().isEmpty()) Items = Queue->Queue()->Items();
    return !Items.isEmpty();
  }));
  CHECK(VisibleProgress(Window) == nullptr);
  if (!Items.isEmpty())
  {
    QStringList Sources;
    for (const QString & S : Items.first().Sources) Sources.append(QFileInfo(S).fileName());
    std::printf("  queued after %lld ms: %s (remote sizes %s)\n", Timer.elapsed(), qPrintable(Sources.join(QStringLiteral(", "))), qPrintable(Sizes()));
    CHECK(Items.first().ToRemote);
    CHECK(!Sources.contains(QStringLiteral("f1.bin")));
    CHECK(Sources.contains(QStringLiteral("f5.bin")));
    CHECK(Sources.size() < 5);
    CHECK(Items.first().TargetDirectory == RemoteDir);
  }
  CHECK(Queue->isVisible());
  // no error about the abort was shown
  CHECK(OtherModals.isEmpty());
  CHECK(WaitFor([&]()
  {
    for (const QString & Name : Names)
    {
      if (QFileInfo(RemoteDir + QLatin1Char('/') + Name).size() != 200 * 1024) return false;
    }
    return Queue->Queue()->ActiveCount() == 0;
  }, 30000));
  CHECK(RightConnected(Window));

  // a directory being transferred is queued whole, with everything after it
  QDir().mkpath(LocalDir + QStringLiteral("/d1"));
  for (const char * Name : {"a.bin", "b.bin", "c.bin"})
  {
    WriteFile(LocalDir + QStringLiteral("/d1/") + QString::fromLatin1(Name), 200 * 1024);
  }
  WriteFile(LocalDir + QStringLiteral("/z.bin"), 1000);
  L->Refresh();
  CHECK(WaitFor([&]() { return !L->IsLoading() && L->FocusFile(QStringLiteral("z.bin")); }));
  SelectNames(L, {QStringLiteral("d1"), QStringLiteral("z.bin")});
  Window.Actions()->Get(QStringLiteral("LocalCopyAction"))->trigger();
  CHECK(WaitFor([&]() { Progress = VisibleProgress(Window); return Progress != nullptr; }));
  if (Progress == nullptr) return;
  CHECK(WaitFor([&]() { return QFileInfo::exists(RemoteDir + QStringLiteral("/d1/a.bin")); }));
  int Before = Queue->Queue()->Items().size();
  ProgressAction(Progress, QStringLiteral("Proceed in &Background"))->trigger();
  CHECK(WaitFor([&]()
  {
    Items = Queue->Queue()->Items();
    return Items.size() > Before;
  }));
  if (Items.size() > Before)
  {
    QStringList Sources;
    for (const QString & S2 : Items.last().Sources) Sources.append(QFileInfo(S2).fileName());
    std::printf("  queued: %s\n", qPrintable(Sources.join(QStringLiteral(", "))));
    CHECK(Sources == (QStringList{QStringLiteral("d1"), QStringLiteral("z.bin")}));
  }
  CHECK(WaitFor([&]()
  {
    return (QFileInfo(RemoteDir + QStringLiteral("/d1/c.bin")).size() == 200 * 1024) &&
      QFileInfo::exists(RemoteDir + QStringLiteral("/z.bin")) && (Queue->Queue()->ActiveCount() == 0);
  }, 30000));
}

static void TestOnceDoneDisconnect(CommanderWindow & Window, const QString & Root)
{
  std::printf("[once finished: disconnect]\n");
  QString RemoteDir = QDir(Root).filePath(QStringLiteral("od-remote"));
  QString LocalDir = QDir(Root).filePath(QStringLiteral("od-local"));
  QDir().mkpath(RemoteDir);
  QDir().mkpath(LocalDir);
  WriteFile(LocalDir + QStringLiteral("/big.bin"), 600 * 1024);
  FilePanel * R = Panel(Window, true);
  FilePanel * L = Panel(Window, false);
  R->ChangeDirectory(RemoteDir);
  L->ChangeDirectory(LocalDir);
  CHECK(WaitFor([&]() { return (R->CurrentDirectory() == RemoteDir) && (L->CurrentDirectory() == LocalDir) && !R->IsLoading(); }));
  Configuration::Instance().DefaultCopyParam().CPSLimit = 300 * 1024;
  SelectNames(L, {QStringLiteral("big.bin")});
  Window.Actions()->Get(QStringLiteral("LocalCopyAction"))->trigger();
  ProgressDialog * Progress = nullptr;
  CHECK(WaitFor([&]() { Progress = VisibleProgress(Window); return Progress != nullptr; }));
  if (Progress == nullptr) return;
  // "Disconnect Session" is offered (the window handles it)
  QAction * Disconnect = ProgressAction(Progress, QStringLiteral("&Disconnect Session"));
  CHECK((Disconnect != nullptr) && Disconnect->isVisible());
  Progress->SetOnceDoneOperation(SystemActions::odoDisconnect);
  MessageBoxTexts.clear();
  MessageBoxAnswer = QMessageBox::No;   // do not close the application
  CHECK(WaitFor([&]() { return !RightConnected(Window); }));
  CHECK(QFileInfo(RemoteDir + QStringLiteral("/big.bin")).size() == 600 * 1024);
  CHECK(WaitFor([&]() { return MessageBoxTexts.contains(QStringLiteral("Operation was successfully completed. Connection was closed.")); }));
  CHECK(Window.isVisible());
  Configuration::Instance().DefaultCopyParam().CPSLimit = 0;
}

static void TestOnceEmptyDisconnect(CommanderWindow & Window, const QString & Root)
{
  std::printf("[once empty: disconnect]\n");
  QString RemoteDir = QDir(Root).filePath(QStringLiteral("oe-remote"));
  QString LocalDir = QDir(Root).filePath(QStringLiteral("oe-local"));
  QDir().mkpath(RemoteDir);
  QDir().mkpath(LocalDir);
  WriteFile(LocalDir + QStringLiteral("/q.bin"), 400 * 1024);
  CHECK(Connect(Window, RemoteDir, LocalDir));
  FilePanel * L = Panel(Window, false);
  CHECK(WaitFor([&]() { return L->CurrentDirectory() == LocalDir; }));
  Configuration::Instance().DefaultCopyParam().CPSLimit = 200 * 1024;
  SelectNames(L, {QStringLiteral("q.bin")});
  Window.Actions()->Get(QStringLiteral("LocalCopyQueueAction"))->trigger();
  QueuePanel * Queue = Window.findChild<QueuePanel *>();
  CHECK(WaitFor([&]() { return (Queue->Queue() != nullptr) && (Queue->Queue()->ActiveCount() > 0); }));
  Queue->SetOnceEmpty(QueuePanel::odoDisconnect);
  MessageBoxTexts.clear();
  MessageBoxAnswer = QMessageBox::No;
  CHECK(WaitFor([&]() { return !RightConnected(Window); }, 30000));
  CHECK(QFileInfo(RemoteDir + QStringLiteral("/q.bin")).size() == 400 * 1024);
  CHECK(WaitFor([&]() { return !MessageBoxTexts.isEmpty(); }));
  CHECK(Queue->OnceEmpty() == QueuePanel::odoIdle);
  CHECK(Window.isVisible());
  Configuration::Instance().DefaultCopyParam().CPSLimit = 0;
}

static void TestUserActionPending(CommanderWindow & Window, const QString & Root)
{
  std::printf("[queue user action]\n");
  QString RemoteDir = QDir(Root).filePath(QStringLiteral("ua-remote"));
  QString LocalDir = QDir(Root).filePath(QStringLiteral("ua-local"));
  QDir().mkpath(RemoteDir);
  QDir().mkpath(LocalDir);
  WriteFile(LocalDir + QStringLiteral("/exists.bin"), 1000);
  WriteFile(RemoteDir + QStringLiteral("/exists.bin"), 10);
  Configuration & Config = Configuration::Instance();
  Config.QueueAutoPopup = false;
  Config.DefaultCopyParam().QueueNoConfirmation = false;
  Config.DefaultCopyParam().Queue = true;
  CHECK(Connect(Window, RemoteDir, LocalDir));
  FilePanel * L = Panel(Window, false);
  CHECK(WaitFor([&]() { return L->CurrentDirectory() == LocalDir; }));
  QueuePanel * Queue = Window.findChild<QueuePanel *>();
  SelectNames(L, {QStringLiteral("exists.bin")});
  OtherModals.clear();
  MessageBoxTexts.clear();
  Window.Actions()->Get(QStringLiteral("LocalCopyAction"))->trigger();
  int Id = 0;
  CHECK(WaitFor([&]() { return (Queue->Queue() != nullptr) && ((Id = Queue->Queue()->UserActionItem()) != 0); }));
  // the item waiting for the user is selected in the (visible) queue list
  auto * View = Queue->findChild<QTreeWidget *>(QStringLiteral("QueueView3"));
  CHECK(Queue->isVisible());
  CHECK(WaitFor([&]() { return (View->currentItem() != nullptr) && (View->currentItem()->data(0, Qt::UserRole).toInt() == Id) && View->currentItem()->isSelected(); }, 3000));
  // no popup without QueueAutoPopup
  QThread::msleep(700);
  QApplication::processEvents();
  CHECK(OtherModals.isEmpty() && MessageBoxTexts.isEmpty());
  CHECK(Queue->Queue()->UserActionItem() == Id);
  // with QueueAutoPopup the query is shown (answered by rejecting it)
  Config.QueueAutoPopup = true;
  MessageBoxTexts.clear();
  MessageBoxAnswer = QMessageBox::Cancel;
  CHECK(WaitFor([&]() { return !OtherModals.isEmpty() || !MessageBoxTexts.isEmpty(); }, 5000));
  std::printf("  popped up: %s %s\n", qPrintable(OtherModals.join(QLatin1Char(','))), qPrintable(MessageBoxTexts.join(QLatin1Char(','))));
  CHECK(WaitFor([&]() { return Queue->Queue()->UserActionItem() == 0; }, 5000));
  Config.DefaultCopyParam().Queue = false;
  Config.DefaultCopyParam().QueueNoConfirmation = true;
}

static void TestApplyConfiguration(CommanderWindow & Window)
{
  std::printf("[preferences apply to queue]\n");
  QueuePanel * Queue = Window.findChild<QueuePanel *>();
  CHECK(Queue->Queue() != nullptr);
  if (Queue->Queue() == nullptr) return;
  Configuration & Config = Configuration::Instance();
  int Old = Queue->Queue()->TransfersLimit();
  int New = (Old == 5) ? 4 : 5;
  // Preferences writes the dialog values on OK; change the limit through the
  // Background page spin box instead of the configuration
  QTimer::singleShot(200, [New]()
  {
    auto * Dialog = qobject_cast<PreferencesDialog *>(QApplication::activeModalWidget());
    if (Dialog == nullptr) return;
    for (QLabel * Label : Dialog->findChildren<QLabel *>())
    {
      if ((Label->text() == QStringLiteral("&Maximal number of transfers at the same time:")) && (qobject_cast<QSpinBox *>(Label->buddy()) != nullptr))
      {
        qobject_cast<QSpinBox *>(Label->buddy())->setValue(New);
      }
    }
    for (QDialogButtonBox * Box : Dialog->findChildren<QDialogButtonBox *>())
    {
      if (Box->button(QDialogButtonBox::Ok) != nullptr) Box->button(QDialogButtonBox::Ok)->click();
    }
  });
  Window.Actions()->Get(QStringLiteral("PreferencesAction"))->trigger();
  CHECK(WaitFor([&]() { return QApplication::activeModalWidget() == nullptr; }, 5000));
  std::printf("  transfers limit %d -> %d (configuration %d)\n", Old, Queue->Queue()->TransfersLimit(), Config.QueueTransfersLimit);
  CHECK(Config.QueueTransfersLimit == New);
  CHECK(Queue->Queue()->TransfersLimit() == New);
}

static QString XdgQuery(const QString & Scheme)
{
  QProcess P;
  P.start(QStringLiteral("xdg-mime"), {QStringLiteral("query"), QStringLiteral("default"), QStringLiteral("x-scheme-handler/") + Scheme});
  P.waitForFinished(10000);
  return QString::fromUtf8(P.readAllStandardOutput()).trimmed();
}

static void TestUrlRegistration(QWidget * Parent, const QString & DataHome, const QString & ConfigHome)
{
  std::printf("[register to handle URL addresses]\n");
  QString DesktopFile = DataHome + QStringLiteral("/applications/nixscp-url-handler.desktop");
  for (bool Register : {true, false})
  {
    QTimer::singleShot(200, [Register]()
    {
      auto * Dialog = qobject_cast<PreferencesDialog *>(QApplication::activeModalWidget());
      if (Dialog == nullptr) return;
      QAction * Item = Dialog->findChild<QAction *>(Register ? QStringLiteral("RegisterAsUrlHandlerItem") : QStringLiteral("UnregisterForDefaultProtocolsItem"));
      CHECK(Item != nullptr);
      MessageBoxAnswer = QMessageBox::Yes;
      MessageBoxTexts.clear();
      if (Item != nullptr) Item->trigger();
      Dialog->reject();
    });
    PreferencesDialog::Execute(Parent, QStringLiteral("Integration"));
    CHECK(MessageBoxTexts.size() == 1);
    CHECK(MessageBoxTexts.value(0) == (Register ? QStringLiteral("Do you want to register NixSCP to handle URL addresses?") :
      QStringLiteral("Do you want to unregister NixSCP from handling all URL addresses?")));
    if (Register)
    {
      QFile F(DesktopFile);
      CHECK(F.open(QIODevice::ReadOnly));
      QString Content = QString::fromUtf8(F.readAll());
      CHECK(Content.contains(QStringLiteral("MimeType=x-scheme-handler/sftp;x-scheme-handler/scp;x-scheme-handler/ftp;"
        "x-scheme-handler/ftps;x-scheme-handler/ftpes;x-scheme-handler/dav;x-scheme-handler/davs;x-scheme-handler/s3;")));
      CHECK(Content.contains(QStringLiteral(" %u\n")));
      for (const char * Scheme : {"sftp", "scp", "ftp", "ftps", "ftpes", "dav", "davs", "s3"})
      {
        CHECK_MSG(XdgQuery(QString::fromLatin1(Scheme)) == QStringLiteral("nixscp-url-handler.desktop"), QString::fromLatin1(Scheme));
      }
      CHECK(QFileInfo::exists(ConfigHome + QStringLiteral("/mimeapps.list")));
    }
    else
    {
      CHECK(!QFileInfo::exists(DesktopFile));
      CHECK(XdgQuery(QStringLiteral("sftp")).isEmpty());
      QFile F(ConfigHome + QStringLiteral("/mimeapps.list"));
      if (F.open(QIODevice::ReadOnly))
      {
        CHECK(!QString::fromUtf8(F.readAll()).contains(QStringLiteral("nixscp-url-handler.desktop")));
      }
    }
  }
  // declined: nothing happens
  QTimer::singleShot(200, []()
  {
    auto * Dialog = qobject_cast<PreferencesDialog *>(QApplication::activeModalWidget());
    if (Dialog == nullptr) return;
    MessageBoxAnswer = QMessageBox::No;
    Dialog->findChild<QAction *>(QStringLiteral("RegisterAsUrlHandlerItem"))->trigger();
    Dialog->reject();
  });
  PreferencesDialog::Execute(Parent, QStringLiteral("Integration"));
  CHECK(!QFileInfo::exists(DesktopFile));
}

int main(int argc, char ** argv)
{
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
  {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  QTemporaryDir Scratch;
  // never touch the user's desktop integration: own XDG homes, generic desktop for xdg-mime
  QString DataHome = Scratch.filePath(QStringLiteral("xdg-data"));
  QString ConfigHome = Scratch.filePath(QStringLiteral("xdg-config"));
  QDir().mkpath(DataHome);
  QDir().mkpath(ConfigHome);
  qputenv("XDG_DATA_HOME", DataHome.toUtf8());
  qputenv("XDG_CONFIG_HOME", ConfigHome.toUtf8());
  for (const char * Name : {"XDG_CURRENT_DESKTOP", "DESKTOP_SESSION", "KDE_FULL_SESSION", "GNOME_DESKTOP_SESSION_ID", "XDG_SESSION_DESKTOP"})
  {
    qunsetenv(Name);
  }
  QApplication App(argc, argv);
  setvbuf(stdout, nullptr, _IONBF, 0);
  Configuration & Config = Configuration::Instance();
  Config.SetIniFileName(Scratch.filePath(QStringLiteral("nixscp.ini")));
  Config.Load();
  Config.ConfirmClosingSession = false;
  Config.TemporaryDirectoryCleanup = false;
  Config.ConfirmExitOnCompletion = true;
  Config.BeepOnFinish = false;
  InstallModalHandler();

  TestUrlRegistration(nullptr, DataHome, ConfigHome);

  if (qEnvironmentVariableIsEmpty("NIXSCP_TEST_SSH_PORT") || qEnvironmentVariableIsEmpty("NIXSCP_TEST_SSH_KEY"))
  {
    std::printf("[session] SKIPPED (set NIXSCP_TEST_SSH_PORT, NIXSCP_TEST_SSH_KEY)\n");
  }
  else
  {
    CommanderWindow Window;
    Window.resize(1100, 750);
    Window.show();
    QApplication::processEvents();
    QString Root = QDir(Scratch.path()).filePath(QStringLiteral("files"));
    QDir().mkpath(Root);
    TestDirectorySizes(Window, Root);
    TestMoveToQueue(Window, Root);
    TestApplyConfiguration(Window);
    TestOnceDoneDisconnect(Window, Root);
    TestOnceEmptyDisconnect(Window, Root);
    TestUserActionPending(Window, Root);
    QString Shot = qEnvironmentVariable("NIXSCP_TEST_SCREENSHOT");
    if (!Shot.isEmpty())
    {
      Window.grab().save(Shot);
    }
  }
  std::printf("%d checks, %d failures\n", Checks, Failures);
  return (Failures == 0) ? 0 : 1;
}

#endif
