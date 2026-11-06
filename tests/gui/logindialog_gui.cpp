// Offscreen GUI check of the Login dialog flows (WP12): New Site URL/port
// parsing, Save = Save As, saved password placeholder, edit + leave prompt,
// Clone to New Site, in-place rename, drag to folder, folder rename,
// incremental search, Paste Session URL, Set Defaults, open all sites of a
// folder, LastStoredSession and the reconnect preselection.
//
// Not built by CMake (tests/test_*.cpp link the core only). Build against a
// configured build directory B (after "cmake --build B --target nixscp"):
//   c++ -std=gnu++20 -fPIC -I B/nixscp_autogen/include -I src $(pkg-config --cflags Qt6Widgets)
//     -c tests/gui/logindialog_gui.cpp -o /tmp/logindialog_gui.o
//   c++ -o /tmp/logindialog_gui /tmp/logindialog_gui.o B/CMakeFiles/nixscp.dir/src/gui/*.o
//     B/CMakeFiles/nixscp.dir/nixscp_autogen/mocs_compilation.cpp.o B/libnixscpcore.a
//     $(pkg-config --libs Qt6Widgets Qt6Network libssh libcrypto)
//   QT_QPA_PLATFORM=offscreen /tmp/logindialog_gui
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QCheckBox>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QFile>
#include <QDir>
#include <QTemporaryDir>
#include <QThread>
#include <QTreeWidget>
#include <QPlainTextEdit>
#include <QToolButton>
#include <QListWidget>
#include <QStackedWidget>
#include <QLabel>
#include <QAction>
#include <QMenu>
#include <QHash>
#include <QIcon>
#include <QKeyEvent>
#include <QClipboard>
#include <QGuiApplication>
#include <cstdio>
#include "core/Configuration.h"
#include "core/IniStorage.h"
#include "core/SiteImport.h"
#define private public
#define protected public
#include "gui/LoginDialog.h"
#undef private
#undef protected

static int Failures = 0;
#define CHECK(Cond) do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); Failures++; } } while (0)

// answers the next modal dialog: accepts message boxes with Button, or runs Fn on a QDialog
static void Answer(std::function<void(QWidget *)> Fn)
{
  QTimer::singleShot(50, [Fn]()
  {
    QWidget * W = QApplication::activeModalWidget();
    if (W == nullptr) { std::printf("no modal\n"); Failures++; return; }
    Fn(W);
  });
}

static void PressBox(QWidget * W, QMessageBox::StandardButton B)
{
  auto * Box = qobject_cast<QMessageBox *>(W);
  if (Box == nullptr) { std::printf("not a message box: %s\n", W->metaObject()->className()); Failures++; static_cast<QDialog *>(W)->reject(); return; }
  Box->button(B)->click();
}

int main(int argc, char ** argv)
{
  // never the real home folder, whatever the caller set
  QTemporaryDir Home;
  qputenv("HOME", Home.path().toUtf8());
  qputenv("XDG_CONFIG_HOME", (Home.path() + "/.config").toUtf8());
  QApplication App(argc, argv);
  QString Ini = qEnvironmentVariable("HOME") + "/test.ini";
  QFile::remove(Ini);
  Configuration & Config = Configuration::Instance();
  Config.SetIniFileName(Ini);
  Config.Load();
  StoredSessionList & Sessions = Config.StoredSessions();
  {
    IniStorage S(&Config.Ini());
    S.OpenSubKey("Configuration", true); S.OpenSubKey("Interface", true);
    S.WriteBool("AutoImportedFromPuttyOrFilezilla", true);
  }
  auto Site = std::make_shared<SessionData>(Sessions.DefaultSettings());
  Site->Name = "Work/alpha"; Site->HostName = "alpha.example.com"; Site->UserName = "a"; Site->Password = "pw";
  Sessions.Add(Site);
  auto NoHost = std::make_shared<SessionData>(Sessions.DefaultSettings());
  NoHost->Name = "nohost"; NoHost->UserName = "x";
  Sessions.Add(NoHost);
  Config.Save();

  {
    LoginDialog D(Sessions);
    D.show();
    QApplication::processEvents();
    // New Site: typing a user@host:port URL and a well-known port
    D.SetSelected(D.NewSiteNode());
    CHECK(D.IsEditable());
    D.HostNameEdit->setText("me@new.example.com:2222");
    D.ParseHostName();
    CHECK(D.HostNameEdit->text() == "new.example.com");
    CHECK(D.UserNameEdit->text() == "me");
    CHECK(D.PortNumberEdit->value() == 2222);
    D.PortNumberEdit->setValue(21);
    CHECK(D.TransferProtocolCombo->currentIndex() == 2); // FTP
    D.PortNumberEdit->setValue(990);
    CHECK(D.GetFtps() == ftpsImplicit);
    D.PortNumberEdit->setValue(22);
    CHECK(D.TransferProtocolCombo->currentIndex() == 0);
    CHECK(D.GetFtps() == ftpsNone);
    // a port typed by the user is kept when the protocol changes
    D.TransferProtocolCombo->setCurrentIndex(3); // WebDAV
    CHECK(D.PortNumberEdit->value() == 22);
    D.TransferProtocolCombo->setCurrentIndex(0);
    // ... otherwise the default port follows the protocol
    D.FEditingData.PortNumber = 22;
    D.LoadSession(D.FEditingData);
    D.TransferProtocolCombo->setCurrentIndex(3); // WebDAV
    CHECK(D.PortNumberEdit->value() == 80);
    D.WebDavsCombo->setCurrentIndex(1);
    CHECK(D.PortNumberEdit->value() == 443);
    D.TransferProtocolCombo->setCurrentIndex(0);
    CHECK(D.PortNumberEdit->value() == 22);
    D.HostNameEdit->setText("me@new.example.com:2222");
    D.ParseHostName();
    D.PasswordEdit->setText("secret");
    // Save on New Site = Save As (dialog); keep password unchecked
    Answer([](QWidget * W)
    {
      auto * Dlg = qobject_cast<QDialog *>(W);
      auto Edits = Dlg->findChildren<QLineEdit *>();
      auto Combos = Dlg->findChildren<QComboBox *>();
      auto Checks = Dlg->findChildren<QCheckBox *>();
      CHECK(Combos.size() == 1);
      CHECK(Combos.first()->currentText() == "<none>");
      CHECK(Checks.size() == 2);
      CHECK(Checks[0]->isEnabled() && !Checks[0]->isChecked());
      CHECK(Edits.first()->text() == "me@new.example.com");
      Combos.first()->setCurrentText("Work");
      Dlg->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    D.SaveSessionActionExecute();
    auto Saved = Sessions.FindByName("Work/me@new.example.com");
    CHECK(Saved != nullptr);
    if (Saved) { CHECK(Saved->Password.isEmpty()); CHECK(Saved->HostName == "new.example.com"); }
    CHECK(D.SessionNodePath(D.Selected()) == "Work/me@new.example.com");
    CHECK(D.PasswordEdit->text().isEmpty());
    // a saved site with a password shows a placeholder
    D.ReloadSessions("Work/alpha");
    CHECK(D.PasswordEdit->text() == QString(16, '?'));
    CHECK(!D.IsEditable());
    // Edit, change, leave: answer "Yes" to save
    D.EditSessionActionExecute();
    CHECK(D.FEditing);
    CHECK(D.PasswordEdit->text() == "pw");
    D.UserNameEdit->setText("a2");
    Answer([](QWidget * W) { PressBox(W, QMessageBox::Yes); });
    CHECK(D.EnsureNotEditing());
    CHECK(!D.FEditing);
    CHECK(Sessions.FindByName("Work/alpha")->UserName == "a2");
    CHECK(Sessions.FindByName("Work/alpha")->Password == "pw");
    // Clone to new site
    Sessions.FindByName("Work/alpha")->AgentAccess = 1;
    Sessions.FindByName("Work/alpha")->AgentName = "prod";
    D.ReloadSessions("Work/alpha");
    D.CloneToNewSite();
    CHECK(D.IsNewSiteNode(D.Selected()));
    CHECK(D.FNewSiteData.Name == "Work/alpha (1)");
    CHECK(D.HostNameEdit->text() == "alpha.example.com");
    // agents do not get at the clone, least of all under the name of the original
    CHECK(D.FNewSiteData.AgentAccess == 0);
    CHECK(D.FNewSiteData.AgentName.isEmpty());
    Sessions.FindByName("Work/alpha")->AgentAccess = 0;
    Sessions.FindByName("Work/alpha")->AgentName.clear();
    // clone is default for sites that cannot be opened
    D.ReloadSessions("nohost");
    CHECK(D.IsCloneToNewSiteDefault());
    CHECK(!D.CanOpen());
    // rename site
    D.ReloadSessions("Work/alpha");
    D.FRenamingOriginal = "alpha";
    D.Selected()->setText(0, "beta");
    D.SessionTreeEdited(D.Selected());
    CHECK(Sessions.FindByName("Work/alpha") == nullptr);
    CHECK(Sessions.FindByName("Work/beta") != nullptr);
    CHECK(D.SessionNodePath(D.Selected()) == "Work/beta");
    // in-place rename with the editor and Enter (must not log in)
    D.RenameSessionActionExecute();
    QApplication::processEvents();
    QLineEdit * Editor = nullptr;
    for (QLineEdit * E : reinterpret_cast<QTreeWidget *>(D.SessionTree)->viewport()->findChildren<QLineEdit *>())
    {
      if (E->isVisible()) Editor = E;
    }
    CHECK(Editor != nullptr);
    if (Editor != nullptr)
    {
      Editor->setText("gamma");
      QKeyEvent Press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
      QApplication::sendEvent(Editor, &Press);
      for (int I = 0; I < 5; I++) QApplication::processEvents();
      CHECK(D.isVisible());
      CHECK(Sessions.FindByName("Work/gamma") != nullptr);
      CHECK(D.SessionNodePath(D.Selected()) == "Work/gamma");
      D.FRenamingOriginal = "gamma";
      D.Selected()->setText(0, "beta");
      D.SessionTreeEdited(D.Selected());
    }
    // drag the site to the root
    D.SessionTreeDropped(D.Selected(), nullptr);
    CHECK(Sessions.FindByName("beta") != nullptr);
    CHECK(Sessions.FindByName("Work/beta") == nullptr);
    // and into the folder again
    D.ReloadSessions("beta");
    QTreeWidgetItem * Work = D.AddSessionPath("Work", false);
    CHECK(Work != nullptr);
    CHECK(D.SessionAllowDrop(Work));
    D.SessionTreeDropped(D.Selected(), Work);
    CHECK(Sessions.FindByName("Work/beta") != nullptr);
    // rename folder
    D.ReloadSessions("Work/");
    CHECK(D.IsFolderNode(D.Selected()));
    CHECK(D.CanOpen());
    CHECK(D.LoginAction->text() == "Open");
    D.FRenamingOriginal = "Work";
    D.Selected()->setText(0, "Job");
    D.SessionTreeEdited(D.Selected());
    CHECK(Sessions.FindByName("Job/beta") != nullptr);
    CHECK(D.SessionNodePath(D.Selected()) == "Job");
    // incremental search: any part of name
    D.SetSiteSearch(isName);
    D.ReloadSessions(QString());
    reinterpret_cast<QTreeWidget *>(D.SessionTree)->collapseAll();
    CHECK(D.SitesIncrementalSearch("eta", false, false, false));
    CHECK(D.SessionNodePath(D.Selected()) == "Job/beta");
    CHECK(D.Selected()->parent()->isExpanded());
    D.SetSiteSearch(isNameStartOnly);
    CHECK(!D.SitesIncrementalSearch("eta", false, false, false));
    D.SetSiteSearch(isAll);
    CHECK(D.SitesIncrementalSearch("alpha.example", false, false, false));
    // Tab: next match (also in collapsed folders)
    D.SetSiteSearch(isName);
    D.ReloadSessions(QString());
    CHECK(D.SitesIncrementalSearch("e", false, false, false));
    QString First = D.SessionNodePath(D.Selected());
    CHECK(D.SitesIncrementalSearch("e", true, false, true));
    CHECK(D.SessionNodePath(D.Selected()) != First);
    D.ResetSitesIncrementalSearch();
    // Paste Session URL
    QGuiApplication::clipboard()->setText("sftp://pu@paste.example.com:2200/srv/");
    QApplication::processEvents();
    D.ReloadSessions("Job/beta");
    D.PasteUrlActionExecute();
    CHECK(D.IsNewSiteNode(D.Selected()));
    CHECK(D.HostNameEdit->text() == "paste.example.com");
    CHECK(D.UserNameEdit->text() == "pu");
    CHECK(D.PortNumberEdit->value() == 2200);
    CHECK(D.FEditingData.RemoteDirectory == "/srv/");
    // S3 captions and host
    D.TransferProtocolCombo->setCurrentIndex(4);
    CHECK(D.UserNameLabel->text() == "A&ccess key ID:");
    CHECK(D.HostNameEdit->text() == "s3.amazonaws.com");
    CHECK(D.PortNumberEdit->value() == 2200);
    D.TransferProtocolCombo->setCurrentIndex(0);
    CHECK(D.HostNameEdit->text().isEmpty());
    // Set Defaults from New Site
    D.SetSelected(D.NewSiteNode());
    D.FNewSiteKeepName = false;
    D.ResetNewSessionActionExecute();
    D.HostNameEdit->setText("");
    D.UserNameEdit->setText("defuser");
    Answer([](QWidget * W) { PressBox(W, QMessageBox::Ok); });
    D.SetDefaultSessionActionExecute();
    CHECK(Sessions.DefaultSettings().UserName == "defuser");
    CHECK(D.UserNameEdit->text() == "defuser");
    // nohost had no own user name -> inherits... it had "x"
    CHECK(Sessions.FindByName("nohost")->UserName == "x");
    // New folder
    D.ReloadSessions("Job/");
    D.FSessions.AddFolder("Job/Sub");
    D.ReloadSessions("Job/Sub/");
    CHECK(D.IsFolderNode(D.Selected()) && D.SessionNodePath(D.Selected()) == "Job/Sub");
    // delete empty folder: no prompt
    D.DeleteSessionActionExecute();
    CHECK(D.AddSessionPath("Job/Sub", false) == nullptr);
    // open all sites of a folder
    D.ReloadSessions("Job/");
    D.LoginActionExecute();
    CHECK(D.result() == QDialog::Accepted);
    CHECK(D.WorkspaceToOpen() == "Job");
    CHECK(D.SessionsToOpen().size() == 2);
  }
  {
    // state remembered
    IniStorage S(&Config.Ini());
    S.OpenSubKey("Configuration", false); S.OpenSubKey("Interface", false);
    CHECK(S.ReadString("LastStoredSession", "") == "Job/");
    LoginDialog D(Sessions);
    D.show();
    QApplication::processEvents();
    CHECK(D.SessionNodePath(D.Selected()) == "Job");
    // reconnect: an ad-hoc session goes to New Site
    SessionData Adhoc = Sessions.DefaultSettings();
    Adhoc.HostName = "adhoc.example.com";
    D.SetInitialSession(Adhoc);
    CHECK(D.IsNewSiteNode(D.Selected()));
    CHECK(D.HostNameEdit->text() == "adhoc.example.com");
    // a stored one is selected
    D.SetInitialSession(*Sessions.FindByName("Job/beta"));
    CHECK(D.SessionNodePath(D.Selected()) == "Job/beta");
    // env vars expanded in the opened session
    qputenv("WP12HOST", "expanded.example.com");
    D.SetSelected(D.NewSiteNode());
    D.HostNameEdit->setText("$WP12HOST");
    D.LoginActionExecute();
    CHECK(D.SessionsToOpen().size() == 1);
    CHECK(D.SessionsToOpen().first().HostName == "expanded.example.com");
    CHECK(D.SessionsToOpen().first().Name.isEmpty());
  }
  {
    // first-run offer to import sites (IMPORT_SESSIONS2) from ~/.ssh/config
    QString Home = qEnvironmentVariable("HOME");
    QDir().mkpath(Home + "/.ssh");
    QFile Ssh(Home + "/.ssh/config");
    CHECK(Ssh.open(QIODevice::WriteOnly));
    Ssh.write("Host imported\n  HostName imported.example.com\n  User imp\n");
    Ssh.close();
    {
      IniStorage S(&Config.Ini());
      S.OpenSubKey("Configuration", true); S.OpenSubKey("Interface", true);
      S.WriteBool("AutoImportedFromPuttyOrFilezilla", false);
    }
    bool Offered = false;
    bool ImportShown = false;
    Answer([&](QWidget * W)
    {
      Offered = (qobject_cast<QMessageBox *>(W) != nullptr);
      Answer([&](QWidget * W2)
      {
        auto * Import = qobject_cast<ImportSessionsDialog *>(W2);
        ImportShown = (Import != nullptr);
        if (Import != nullptr)
        {
          CHECK(Import->SourceIndex() == SiteImport::isOpenssh);
          Import->accept();
        }
        else
        {
          static_cast<QDialog *>(W2)->reject();
        }
      });
      PressBox(W, QMessageBox::Yes);
    });
    LoginDialog D(Sessions);
    D.show();
    for (int I = 0; I < 20; I++) { QApplication::processEvents(); QThread::msleep(20); }
    CHECK(Offered);
    CHECK(ImportShown);
    auto Imported = Sessions.FindByName("imported");
    CHECK(Imported != nullptr);
    if (Imported) { CHECK(Imported->HostName == "imported.example.com"); CHECK(Imported->UserName == "imp"); }
    CHECK(D.FindSessionNode("imported") != nullptr);
    D.reject();
  }
  std::printf(Failures ? "guitest: %d failures\n" : "guitest: all passed\n", Failures);
  return Failures ? 1 : 0;
}
