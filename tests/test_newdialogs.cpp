// The dialogs of the features NixSCP adds to WinSCP, driven like a user:
//  - UnlockDialog: the startup prompt of an encrypted configuration (wrong
//    password, the password, the recovery key, Quit) and the import of an
//    encrypted backup made with another password;
//  - TunnelsDialog: the saved tunnels of SSH sites only, Add / duplicate name
//    / Edit / Remove (saved to the configuration), and Start of a tunnel whose
//    server is unreachable (reported as failed, not hanging);
//  - TeamsDialog: without a NixSCP Pro sign-in it explains how to start and
//    disables the team actions.
// NIXSCP_GUI_TESTS
#ifndef NIXSCP_GUI_TESTS

#include <cstdio>

int main()
{
  std::printf("SKIP: test_newdialogs needs NIXSCP_GUI_TESTS (Qt Widgets + the GUI sources)\n");
  return 0;
}

#else

#include "core/CloudCrypto.h"
#include "core/ConfigVault.h"
#include "core/Configuration.h"
#include "core/PortForward.h"
#include "core/SessionData.h"
#include "gui/TeamsDialog.h"
#include "gui/TunnelsDialog.h"
#include "gui/UnlockDialog.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QListWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>

#include <cstdio>
#include <functional>
#include <memory>

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)

// Called with each modal dialog (polled); returns true when it handled it
static std::function<bool(QWidget *)> ModalHandler;
static QStringList Seen;

static void InstallModalHandler()
{
  auto * Timer = new QTimer(qApp);
  QObject::connect(Timer, &QTimer::timeout, []()
  {
    QWidget * Modal = QApplication::activeModalWidget();
    if ((Modal == nullptr) || (ModalHandler && ModalHandler(Modal)))
    {
      return;
    }
    // unexpected: close it so that the test does not hang
    std::printf("INFO closing unexpected modal: %s\n", qPrintable(Modal->windowTitle()));
    if (auto * Dialog = qobject_cast<QDialog *>(Modal))
    {
      Dialog->reject();
    }
  });
  Timer->start(30);
}

static void Wait(int Ms)
{
  QElapsedTimer Timer;
  Timer.start();
  while (Timer.elapsed() < Ms)
  {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
  }
}

static bool WaitFor(const std::function<bool()> & Condition, int Ms)
{
  QElapsedTimer Timer;
  Timer.start();
  while (!Condition())
  {
    if (Timer.elapsed() > Ms)
    {
      return false;
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
  }
  return true;
}

static QAbstractButton * Button(QWidget * Parent, const QString & Text)
{
  for (QAbstractButton * B : Parent->findChildren<QAbstractButton *>())
  {
    if (B->text().remove(QLatin1Char('&')) == Text)
    {
      return B;
    }
  }
  return nullptr;
}

static void Click(QWidget * Parent, const QString & Text)
{
  QAbstractButton * B = Button(Parent, Text);
  CHECK(B != nullptr);
  if (B != nullptr)
  {
    B->click();
  }
}

static QString VisibleError(QWidget * Dialog)
{
  for (QLabel * Label : Dialog->findChildren<QLabel *>())
  {
    if (Label->isVisible() && Label->styleSheet().contains(QStringLiteral("color")))
    {
      return Label->text();
    }
  }
  return QString();
}

//---------------------------------------------------------------------------
static void TestUnlock(const QString & Dir)
{
  std::printf("-- unlock\n");
  Configuration & Config = Configuration::Instance();
  QString Ini = Dir + QStringLiteral("/nixscp.ini");
  QByteArray Dek = CloudCrypto::RandomBytes(CloudCrypto::KeyLength);
  QByteArray Recovery = CloudCrypto::NewRecoveryKey();
  CloudCrypto::SnapshotHeader Wrap;
  Wrap.User = QStringLiteral("user-1");
  Wrap.Kdf.MemoryKib = 8 * 1024;
  Wrap.Kdf.Iterations = 1;
  CHECK(CloudCrypto::WrapDek(Wrap, Dek, QStringLiteral("correct horse"), Recovery));
  ConfigVault::SetKey(Dek, Wrap);
  CHECK(Config.Rewrite());
  CHECK(ConfigVault::IsSealedFile(Ini));
  QByteArray Backup = ConfigVault::SealExport(QByteArrayLiteral("[Test]\nKey=backup value\n"));

  // wrong password: the dialog stays with an error; then the password unlocks
  ConfigVault::Forget();
  int Step = 0;
  QString Error;
  ModalHandler = [&](QWidget * Modal)
  {
    auto * Edit = Modal->findChild<QLineEdit *>();
    if ((Edit == nullptr) || !Modal->windowTitle().contains(QStringLiteral("NixSCP")))
    {
      return false;
    }
    if (Step == 0)
    {
      Edit->setText(QStringLiteral("wrong password"));
      Click(Modal, QStringLiteral("Unlock"));
      Error = VisibleError(Modal);
      Step = 1;
    }
    else if (Step == 1)
    {
      Edit->setText(QStringLiteral("correct horse"));
      Click(Modal, QStringLiteral("Unlock"));
      Step = 2;
    }
    return true;
  };
  CHECK(UnlockDialog::UnlockConfiguration(Ini));
  CHECK(Step == 2);
  CHECK(!Error.isEmpty());
  CHECK(ConfigVault::Key() == Dek);

  // the recovery key
  ConfigVault::Forget();
  ModalHandler = [&](QWidget * Modal)
  {
    auto * Edit = Modal->findChild<QLineEdit *>();
    auto * Recover = Modal->findChild<QCheckBox *>();
    if ((Edit == nullptr) || (Recover == nullptr))
    {
      return false;
    }
    Recover->setChecked(true);
    CHECK(Edit->echoMode() == QLineEdit::Normal);
    Edit->setText(CloudCrypto::FormatRecoveryKey(Recovery));
    Click(Modal, QStringLiteral("Unlock"));
    return true;
  };
  CHECK(UnlockDialog::UnlockConfiguration(Ini));
  CHECK(ConfigVault::Key() == Dek);

  // Quit leaves it locked
  ConfigVault::Forget();
  ModalHandler = [](QWidget * Modal)
  {
    if (Button(Modal, QStringLiteral("Quit")) == nullptr)
    {
      return false;
    }
    Click(Modal, QStringLiteral("Quit"));
    return true;
  };
  CHECK(!UnlockDialog::UnlockConfiguration(Ini));
  CHECK(ConfigVault::Key().isEmpty());

  // an encrypted backup of another key: asks for its password
  ConfigVault::SetKey(CloudCrypto::RandomBytes(CloudCrypto::KeyLength), Wrap);
  int Asked = 0;
  ModalHandler = [&](QWidget * Modal)
  {
    auto * Edit = Modal->findChild<QLineEdit *>();
    if ((Edit == nullptr) || (Modal->windowTitle() != QStringLiteral("Import Configuration")))
    {
      return false;
    }
    Asked++;
    Edit->setText(QStringLiteral("correct horse"));
    Click(Modal, QStringLiteral("Import"));
    return true;
  };
  QByteArray Plain;
  CHECK(UnlockDialog::OpenEncryptedBackup(nullptr, Dir + QStringLiteral("/backup.ini"), Backup, Plain));
  CHECK(Asked == 1);
  CHECK(Plain.contains("backup value"));
  // Cancel
  ModalHandler = [](QWidget * Modal)
  {
    if (Button(Modal, QStringLiteral("Cancel")) == nullptr)
    {
      return false;
    }
    Click(Modal, QStringLiteral("Cancel"));
    return true;
  };
  Plain.clear();
  CHECK(!UnlockDialog::OpenEncryptedBackup(nullptr, Dir + QStringLiteral("/backup.ini"), Backup, Plain));
  // a backup of the current key opens without asking
  ConfigVault::SetKey(Dek, Wrap);
  Asked = 0;
  ModalHandler = [&Asked](QWidget *) { Asked++; return false; };
  CHECK(UnlockDialog::OpenEncryptedBackup(nullptr, Dir + QStringLiteral("/backup.ini"), Backup, Plain));
  CHECK(Asked == 0);
  ModalHandler = nullptr;
  // back to a plain file (Disable keeps the key to read the encrypted one)
  ConfigVault::Disable();
  CHECK(Config.Rewrite());
  CHECK(!ConfigVault::IsSealedFile(Ini));
  ConfigVault::Forget();
}

//---------------------------------------------------------------------------
static QStringList Column(QTableWidget * Table, int C)
{
  QStringList Result;
  for (int R = 0; R < Table->rowCount(); R++)
  {
    Result.append(Table->item(R, C)->text());
  }
  return Result;
}

static void SelectRow(QTableWidget * Table, const QString & Name)
{
  for (int R = 0; R < Table->rowCount(); R++)
  {
    if (Table->item(R, 1)->text() == Name)
    {
      Table->selectRow(R);
    }
  }
}

static void FillTunnel(QWidget * Modal, const QString & Name, int Local, const QString & Host, int Port)
{
  QList<QLineEdit *> Edits = Modal->findChildren<QLineEdit *>();
  QList<QSpinBox *> Spins = Modal->findChildren<QSpinBox *>();
  CHECK((Edits.size() >= 2) && (Spins.size() == 2));
  // the combo box's line edit is not editable: name and host are the plain ones
  QList<QLineEdit *> Plain;
  for (QLineEdit * E : Edits)
  {
    if (qobject_cast<QWidget *>(E->parent()) == Modal)
    {
      Plain.append(E);
    }
  }
  CHECK(Plain.size() == 2);
  Plain.at(0)->setText(Name);
  Spins.at(0)->setValue(Local);
  Plain.at(1)->setText(Host);
  Spins.at(1)->setValue(Port);
  Click(Modal, QStringLiteral("OK"));
}

static void TestTunnels()
{
  std::printf("-- tunnels\n");
  Configuration & Config = Configuration::Instance();
  // a port nothing listens on: the tunnel's SSH connection is refused
  QTcpServer Probe;
  CHECK(Probe.listen(QHostAddress::LocalHost));
  int DeadPort = Probe.serverPort();
  Probe.close();
  SessionData Ssh;
  Ssh.Name = QStringLiteral("db/primary");
  Ssh.HostName = QStringLiteral("127.0.0.1");
  Ssh.PortNumber = DeadPort;
  Ssh.UserName = QStringLiteral("user");
  Ssh.FSProtocol = fsSFTP;
  Ssh.PortForwards = QStringLiteral("postgres|15433|localhost|5432");
  Config.StoredSessions().Add(std::make_shared<SessionData>(Ssh));
  SessionData Ftp;
  Ftp.Name = QStringLiteral("ftp site");
  Ftp.HostName = QStringLiteral("127.0.0.1");
  Ftp.FSProtocol = fsFTP;
  Ftp.PortForwards = QStringLiteral("ignored|15434|localhost|21");
  Config.StoredSessions().Add(std::make_shared<SessionData>(Ftp));
  CHECK(Config.Save());

  TunnelsDialog Dialog;
  Dialog.show();
  auto * Table = Dialog.findChild<QTableWidget *>();
  CHECK(Table != nullptr);
  // SSH sites only
  CHECK(Column(Table, 1) == QStringList{QStringLiteral("postgres")});
  CHECK(Table->item(0, 0)->text() == QStringLiteral("db/primary"));
  CHECK(Table->item(0, 3)->text() == QStringLiteral("localhost:5432"));
  CHECK(Table->item(0, 4)->text() == QStringLiteral("Stopped"));
  CHECK(!Button(&Dialog, QStringLiteral("Start"))->isEnabled());

  // Add
  ModalHandler = [](QWidget * Modal)
  {
    if (Modal->windowTitle() != QStringLiteral("Add Tunnel"))
    {
      return false;
    }
    FillTunnel(Modal, QStringLiteral("redis"), 16379, QStringLiteral("10.0.0.5"), 6379);
    return true;
  };
  Click(&Dialog, QStringLiteral("Add..."));
  CHECK(Column(Table, 1).contains(QStringLiteral("redis")));
  CHECK(Config.StoredSessions().FindByName(Ssh.Name)->PortForwards.contains(QStringLiteral("redis|16379|10.0.0.5|6379")));

  // a second tunnel with the same name is refused
  bool Warned = false;
  ModalHandler = [&Warned](QWidget * Modal)
  {
    if (auto * Box = qobject_cast<QMessageBox *>(Modal))
    {
      Warned = Box->text().contains(QStringLiteral("already has a tunnel"));
      Box->accept();
      return true;
    }
    if (Modal->windowTitle() != QStringLiteral("Add Tunnel"))
    {
      return false;
    }
    FillTunnel(Modal, QStringLiteral("REDIS"), 16380, QStringLiteral("localhost"), 6379);
    return true;
  };
  Click(&Dialog, QStringLiteral("Add..."));
  CHECK(Warned);
  CHECK(Table->rowCount() == 2);

  // Edit
  SelectRow(Table, QStringLiteral("redis"));
  ModalHandler = [](QWidget * Modal)
  {
    if (Modal->windowTitle() != QStringLiteral("Edit Tunnel"))
    {
      return false;
    }
    FillTunnel(Modal, QStringLiteral("cache"), 16381, QStringLiteral("10.0.0.6"), 6380);
    return true;
  };
  Click(&Dialog, QStringLiteral("Edit..."));
  QString Saved = Config.StoredSessions().FindByName(Ssh.Name)->PortForwards;
  CHECK(Saved.contains(QStringLiteral("cache|16381|10.0.0.6|6380")) && !Saved.contains(QStringLiteral("redis")));
  CHECK(Saved.contains(QStringLiteral("postgres|15433|localhost|5432")));

  // Remove (confirmed)
  SelectRow(Table, QStringLiteral("cache"));
  ModalHandler = [](QWidget * Modal)
  {
    auto * Box = qobject_cast<QMessageBox *>(Modal);
    if (Box == nullptr)
    {
      return false;
    }
    Box->button(QMessageBox::Yes)->click();
    return true;
  };
  Click(&Dialog, QStringLiteral("Remove"));
  CHECK(Column(Table, 1) == QStringList{QStringLiteral("postgres")});
  CHECK(!Config.StoredSessions().FindByName(Ssh.Name)->PortForwards.contains(QStringLiteral("cache")));

  // the Tunnels menu of the main window lists the saved tunnels; the status
  // bar indicator is hidden while none runs
  QAction Manage(QStringLiteral("&Manage Tunnels..."));
  QMenu Menu;
  TunnelsUi::SetupMenu(&Menu, &Manage);
  QWidget Window;
  QToolButton * Indicator = TunnelsUi::CreateIndicator(&Window);
  CHECK(Indicator->isHidden());
  emit Menu.aboutToShow();
  QStringList Items;
  for (QAction * A : Menu.actions())
  {
    Items.append(A->isSeparator() ? QStringLiteral("-") : A->text() + (A->isChecked() ? QStringLiteral(" [x]") : QString()));
  }
  CHECK(Items == QStringList({QStringLiteral("&Manage Tunnels..."), QStringLiteral("-"),
    QStringLiteral("db/primary: postgres  (15433 -> localhost:5432)"), QStringLiteral("-"), QStringLiteral("&Stop All Tunnels")}));
  CHECK(!Menu.actions().last()->isEnabled());

  // Start from the menu: shown as running (indicator visible) until it fails
  bool SawIndicator = false;
  QObject::connect(&TunnelManager::Instance(), &TunnelManager::Changed, &Window, [&]()
  {
    SawIndicator = SawIndicator || (!Indicator->isHidden() && Indicator->text().contains(QStringLiteral("1 tunnel running")) &&
      Indicator->toolTip().contains(QStringLiteral("db/primary: postgres")));
  });
  Menu.actions().at(2)->trigger();
  CHECK(SawIndicator);
  CHECK(WaitFor([Table]() { return Table->item(0, 4)->text().startsWith(QStringLiteral("Failed")); }, 30000));
  CHECK(Indicator->isHidden());
  emit Menu.aboutToShow();
  CHECK(!Menu.actions().at(2)->isChecked());
  CHECK(Menu.actions().at(2)->toolTip().startsWith(QStringLiteral("Failed")));

  // Start: the server is unreachable, so it ends as failed
  ModalHandler = nullptr;
  SelectRow(Table, QStringLiteral("postgres"));
  CHECK(Button(&Dialog, QStringLiteral("Start"))->isEnabled());
  Click(&Dialog, QStringLiteral("Start"));
  CHECK(WaitFor([Table]() { return Table->item(0, 4)->text().startsWith(QStringLiteral("Failed")); }, 30000));
  std::printf("INFO status: %s\n", qPrintable(Table->item(0, 4)->text()));
  CHECK(TunnelManager::Instance().Find(TunnelManager::Key(Ssh.Name, QStringLiteral("postgres"))) == nullptr);
  CHECK(Button(&Dialog, QStringLiteral("Start"))->isEnabled());
  CHECK(!Button(&Dialog, QStringLiteral("Stop"))->isEnabled());
  Dialog.close();
}

//---------------------------------------------------------------------------
static void TestTeams()
{
  std::printf("-- teams\n");
  TeamsDialog Dialog;
  Dialog.show();
  Wait(100);
  bool SignIn = false;
  for (QLabel * Label : Dialog.findChildren<QLabel *>())
  {
    SignIn = SignIn || Label->text().contains(QStringLiteral("Sign in to NixSCP Pro"));
  }
  CHECK(SignIn);
  for (const QString & Caption : {QStringLiteral("New Team..."), QStringLiteral("Join with Code..."), QStringLiteral("Sync Now")})
  {
    QAbstractButton * B = Button(&Dialog, Caption);
    CHECK((B != nullptr) && !B->isEnabled());
  }
  // no team: the team details (shared sites, members, invites) are hidden
  QListWidget * Teams = Dialog.findChild<QListWidget *>();
  CHECK((Teams != nullptr) && (Teams->count() == 0));
  QTabWidget * Tabs = Dialog.findChild<QTabWidget *>();
  CHECK((Tabs != nullptr) && !Tabs->isVisible());
  // a team's shared sites, members and invites are tabs of the dialog
  CHECK((Tabs != nullptr) && (Tabs->count() == 3) && Tabs->tabText(0).startsWith(QStringLiteral("Shared Sites")));
  Dialog.close();
}

int main(int argc, char ** argv)
{
  qputenv("NIXSCP_NO_UPDATE_CHECK", "1");
  // the tunnel test connects, and libssh reads ~/.ssh
  QTemporaryDir Dir;
  qputenv("HOME", Dir.path().toUtf8());
  qputenv("XDG_CONFIG_HOME", (Dir.path() + QStringLiteral("/.config")).toUtf8());
  QApplication App(argc, argv);
  Configuration & Config = Configuration::Instance();
  Config.SetIniFileName(Dir.path() + QStringLiteral("/nixscp.ini"));
  Config.Load();
  InstallModalHandler();

  TestUnlock(Dir.path());
  TestTunnels();
  TestTeams();

  std::printf("%s (%d failures)\n", Failures ? "FAILED" : "OK", Failures);
  return Failures ? 1 : 0;
}

#endif
