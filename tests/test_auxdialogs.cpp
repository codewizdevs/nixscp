// Auxiliary dialogs (WP20): gui/HistoryComboBox (Configuration\History),
// gui/HelpSystem (F1, Help buttons, Help menu), gui/AboutDialog (+ License),
// gui/OpenDirectoryDialog and the UseLocationProfiles dispatch of
// gui/LocationProfilesDialog (Ctrl+O / Ctrl+B, add-bookmark confirmation,
// switching, shared bookmark shortcuts, core/Bookmarks::OpenBookmarkTarget),
// gui/GenerateUrlDialog (session URL, nixscp-cli script, shell script,
// command line, remembered options), gui/ServerInfoDialog and
// gui/ConsoleDialog (ConsoleWin\WindowSize, AnyCommand for FTP).
// Runs offscreen. Server parts are skipped without NIXSCP_TEST_SSH_PORT +
// NIXSCP_TEST_SSH_KEY (SFTP: server info, generated script run by nixscp-cli)
// and NIXSCP_TEST_FTP_PORT (FTP console). NIXSCP_TEST_SCREENSHOT_DIR=<dir>
// saves PNGs of the dialogs. NIXSCP_TEST_CLI=<nixscp-cli> runs the generated code.
#ifndef NIXSCP_GUI_TESTS

#include <cstdio>

int main()
{
  std::printf("SKIP: test_auxdialogs needs NIXSCP_GUI_TESTS (Qt Widgets + the GUI sources)\n");
  return 0;
}

#else

#include "core/Bookmarks.h"
#include "core/Configuration.h"
#include "core/IniStorage.h"
#include "core/SessionData.h"
#include "core/Terminal.h"
#include "gui/AboutDialog.h"
#include "gui/ConsoleDialog.h"
#include "gui/EditMaskDialog.h"
#include "gui/GenerateUrlDialog.h"
#include "gui/GuiUserInterface.h"
#include "gui/HelpSystem.h"
#include "gui/HistoryComboBox.h"
#include "gui/LocationProfilesDialog.h"
#include "gui/LoginDialog.h"
#include "gui/OpenDirectoryDialog.h"
#include "gui/ServerInfoDialog.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QGroupBox>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMutex>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTimer>
#include <QTreeWidget>

#include <cstdio>
#include <functional>

static int Failures = 0;
static QString ScreenshotDir;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_EQ(A, B) \
  do { auto VA = (A); auto VB = (B); if (!(VA == VB)) { std::printf("FAIL %s:%d: %s == %s (\"%s\" vs \"%s\")\n", __FILE__, __LINE__, #A, #B, \
    qPrintable(QVariant::fromValue(VA).toString()), qPrintable(QVariant::fromValue(VB).toString())); std::fflush(stdout); Failures++; } } while (0)

static void Screenshot(QWidget * Widget, const QString & Name)
{
  if (!ScreenshotDir.isEmpty())
  {
    Widget->grab().save(QDir(ScreenshotDir).filePath(Name + QStringLiteral(".png")));
  }
}

// Runs Action on the next modal dialog matching Match (polls the event loop)
static void OnModal(std::function<bool(QWidget *)> Match, std::function<void(QWidget *)> Action, int TimeoutMs = 5000)
{
  auto * Elapsed = new int(0);
  auto * Timer = new QTimer();
  Timer->setInterval(20);
  QObject::connect(Timer, &QTimer::timeout, [Timer, Elapsed, Match, Action, TimeoutMs]()
  {
    *Elapsed += 20;
    QWidget * Modal = QApplication::activeModalWidget();
    if ((Modal != nullptr) && Match(Modal))
    {
      Timer->stop();
      Timer->deleteLater();
      delete Elapsed;
      Action(Modal);
    }
    else if (*Elapsed > TimeoutMs)
    {
      std::printf("FAIL: expected modal dialog did not appear\n");
      Failures++;
      Timer->stop();
      Timer->deleteLater();
      delete Elapsed;
      if (Modal != nullptr)
      {
        Modal->close();
      }
    }
  });
  Timer->start();
}

template<typename T>
static std::function<bool(QWidget *)> IsA()
{
  return [](QWidget * W) { return qobject_cast<T *>(W) != nullptr; };
}

static std::function<bool(QWidget *)> Named(const QString & Name)
{
  return [Name](QWidget * W) { return W->objectName() == Name; };
}

static QPushButton * FindButton(QWidget * Parent, const QString & Text)
{
  for (QPushButton * Button : Parent->findChildren<QPushButton *>())
  {
    if (Button->text() == Text)
    {
      return Button;
    }
  }
  return nullptr;
}

static void ProcessEvents(int Ms = 50)
{
  QEventLoop Loop;
  QTimer::singleShot(Ms, &Loop, &QEventLoop::quit);
  Loop.exec();
}

class UrlCatcher : public QObject
{
  Q_OBJECT
public:
  QList<QUrl> Urls;
public slots:
  void Open(const QUrl & Url) { Urls.append(Url); }
};

//---------------------------------------------------------------------------
static void TestHistory()
{
  std::printf("[history]\n");
  IniFile & Ini = Configuration::Instance().Ini();
  CHECK(HistoryComboBox::History(QStringLiteral("TestKey")).isEmpty());
  CHECK_EQ(HistoryComboBox::History(QStringLiteral("TestKey"), { QStringLiteral("d1") }), QStringList({ QStringLiteral("d1") }));
  HistoryComboBox::SaveToHistory(QStringLiteral("TestKey"), QStringLiteral("one"));
  HistoryComboBox::SaveToHistory(QStringLiteral("TestKey"), QStringLiteral("two"));
  HistoryComboBox::SaveToHistory(QStringLiteral("TestKey"), QStringLiteral("ONE"));
  // most recent first, case-insensitive duplicates removed (TComboBox Items->IndexOf)
  CHECK_EQ(HistoryComboBox::History(QStringLiteral("TestKey")), QStringList({ QStringLiteral("ONE"), QStringLiteral("two") }));
  CHECK_EQ(Ini.ReadRaw(QStringLiteral("Configuration\\History\\TestKey"), QStringLiteral("0")), QStringLiteral("ONE"));
  CHECK_EQ(Ini.ReadRaw(QStringLiteral("Configuration\\History\\TestKey"), QStringLiteral("1")), QStringLiteral("two"));
  // once stored, the defaults do not apply
  CHECK_EQ(HistoryComboBox::History(QStringLiteral("TestKey"), { QStringLiteral("d1") }).size(), 2);
  for (int Index = 0; Index < 40; Index++)
  {
    HistoryComboBox::SaveToHistory(QStringLiteral("TestKey"), QStringLiteral("v%1").arg(Index));
  }
  QStringList Items = HistoryComboBox::History(QStringLiteral("TestKey"));
  CHECK_EQ(Items.size(), HistoryComboBox::MaxHistorySize);
  CHECK_EQ(Items.first(), QStringLiteral("v39"));
  CHECK(!Ini.ReadRaw(QStringLiteral("Configuration\\History\\TestKey"), QStringLiteral("30")).size());
  // values are munged as in WinSCP.ini
  HistoryComboBox::SaveToHistory(QStringLiteral("Special"), QStringLiteral("a b/c%d"));
  CHECK_EQ(HistoryComboBox::History(QStringLiteral("Special")).value(0), QStringLiteral("a b/c%d"));

  // widget
  HistoryComboBox Combo(QStringLiteral("WidgetKey"), nullptr);
  CHECK(Combo.isEditable());
  CHECK_EQ(Combo.count(), 0);
  Combo.setEditText(QStringLiteral("first"));
  Combo.SaveToHistory();
  Combo.setEditText(QStringLiteral("second"));
  Combo.SaveToHistory();
  CHECK_EQ(Combo.count(), 2);
  CHECK_EQ(Combo.itemText(0), QStringLiteral("second"));
  CHECK_EQ(Combo.currentText(), QStringLiteral("second"));
  HistoryComboBox Other(QStringLiteral("WidgetKey"), nullptr);
  CHECK_EQ(Other.count(), 2);
  Combo.setEditText(QString());
  Combo.SaveToHistory();   // empty text is not stored
  CHECK_EQ(HistoryComboBox::History(QStringLiteral("WidgetKey")).size(), 2);

  // EditMaskDialog keeps its API, now on top of HistoryComboBox
  EditMaskDialog::SaveToHistory(QStringLiteral("Mask"), QStringLiteral("*.txt"));
  CHECK_EQ(HistoryComboBox::History(QStringLiteral("Mask")).value(0), QStringLiteral("*.txt"));
  CHECK_EQ(EditMaskDialog::History(QStringLiteral("Mask")).value(0), QStringLiteral("*.txt"));
}

//---------------------------------------------------------------------------
static void TestHelp()
{
  std::printf("[help]\n");
  CHECK_EQ(HelpSystem::KeywordUrl(QStringLiteral("ui_opendir")).toString(), QStringLiteral("https://winscp.net/eng/docs/ui_opendir"));
  CHECK_EQ(HelpSystem::KeywordUrl(QStringLiteral("task_navigate#bookmarks")).toString(),
    QStringLiteral("https://winscp.net/eng/docs/task_navigate#bookmarks"));
  CHECK_EQ(HelpSystem::DocumentationUrl().toString(), QStringLiteral("https://winscp.net/eng/docs/"));

  UrlCatcher Catcher;
  QDesktopServices::setUrlHandler(QStringLiteral("https"), &Catcher, "Open");
  QList<QUrl> HelpUrls;
  SetOpenUrlHandler([&HelpUrls](const QUrl & Url) { HelpUrls.append(Url); });

  QMainWindow Window;
  QMenu * Menu = new QMenu(QStringLiteral("&Help"), &Window);
  HelpSystem::PopulateHelpMenu(Menu, &Window);
  QStringList Names;
  QStringList Texts;
  for (QAction * Action : Menu->actions())
  {
    Names << (Action->isSeparator() ? QStringLiteral("-") : Action->objectName());
    Texts << Action->text();
  }
  CHECK_EQ(Names.join(QLatin1Char(',')), QStringLiteral("TableOfContentsAction,-,HomepageAction,IssuesPageAction,ForumPageAction,HistoryPageAction,-,AboutAction"));
  CHECK(!Texts.join(QLatin1Char(',')).contains(QStringLiteral("Donate")));
  QAction * Contents = Menu->actions().value(0);
  CHECK_EQ(Contents->text(), QStringLiteral("&Contents"));
  CHECK(Contents->shortcut() == QKeySequence(Qt::Key_F1));
  CHECK(Window.actions().contains(Contents));
  Contents->trigger();
  Menu->actions().value(2)->trigger();
  Menu->actions().value(3)->trigger();
  Menu->actions().value(4)->trigger();
  Menu->actions().value(5)->trigger();
  // NixSCP: Homepage is nixscp.com, bugs go to GitHub, "Send Feedback..." opens a
  // mailto: link (handled by the mail client), the history is the NixSCP releases.
  // The documentation stays WinSCP's (same interface). Every URL goes through the
  // SetOpenUrlHandler test hook (not QDesktopServices).
  CHECK_EQ(Catcher.Urls.size(), 0);
  CHECK_EQ(HelpUrls.size(), 5);
  CHECK_EQ(HelpUrls.value(0).toString(), QStringLiteral("https://winscp.net/eng/docs/"));
  CHECK_EQ(HelpUrls.value(1).toString(), QStringLiteral("https://nixscp.com/"));
  CHECK_EQ(HelpUrls.value(2).toString(), QStringLiteral("https://github.com/codewizdevs/nixscp/issues"));
  CHECK_EQ(HelpUrls.value(3).scheme(), QStringLiteral("mailto"));
  CHECK_EQ(HelpUrls.value(4).toString(), QStringLiteral("https://github.com/codewizdevs/nixscp/releases"));
  HelpUrls.clear();
  CHECK_EQ(HelpSystem::ContactUrl().scheme(), QStringLiteral("mailto"));
  // About... opens the About dialog
  OnModal(IsA<AboutDialog>(), [](QWidget * W) { W->close(); });
  Menu->actions().value(7)->trigger();

  // dialog Help buttons and F1
  QDialog Dialog;
  QDialogButtonBox * Buttons = new QDialogButtonBox(QDialogButtonBox::Ok, &Dialog);
  QPushButton * HelpButton = HelpSystem::AddHelpButton(Buttons, &Dialog, QStringLiteral("ui_find"));
  CHECK_EQ(HelpButton->text(), QStringLiteral("&Help"));
  HelpButton->click();
  CHECK_EQ(HelpUrls.value(0).toString(), QStringLiteral("https://winscp.net/eng/docs/ui_find"));
  Dialog.show();
  Dialog.activateWindow();
  QKeyEvent F1(QEvent::KeyPress, Qt::Key_F1, Qt::NoModifier);
  QApplication::sendEvent(&Dialog, &F1);
  ProcessEvents();
  Dialog.hide();
  CHECK_EQ(Dialog.property("HelpKeyword").toString(), QStringLiteral("ui_find"));

  // Help in the dialogs of this package
  HelpUrls.clear();
  {
    bool Apply = false;
    QString MaskStr = QStringLiteral("*.x");
    OnModal(IsA<EditMaskDialog>(), [](QWidget * W)
    {
      QPushButton * Help = FindButton(W, QStringLiteral("&Help"));
      CHECK(Help != nullptr);
      if (Help != nullptr) Help->click();
      W->close();
    });
    EditMaskDialog::Execute(nullptr, EditMaskDialog::Filter, MaskStr, Apply);
    CHECK_EQ(HelpUrls.value(0).toString(), QStringLiteral("https://winscp.net/eng/docs/ui_filter"));
  }
  SetOpenUrlHandler(nullptr);
  QDesktopServices::unsetUrlHandler(QStringLiteral("https"));
}

//---------------------------------------------------------------------------
static void TestAbout()
{
  std::printf("[about]\n");
  AboutDialog Dialog;
  CHECK(Dialog.windowTitle().startsWith(QStringLiteral("About ")));
  QString Third = AboutDialog::ThirdPartyText(false);
  CHECK(Third.contains(QStringLiteral("Qt %1").arg(QString::fromLatin1(qVersion()))));
  CHECK(Third.contains(QStringLiteral("libssh 0.")));
  CHECK(Third.contains(QStringLiteral("OpenSSL Toolkit 3.")));
  CHECK(Third.contains(QStringLiteral("Martin Prikryl")));
  CHECK(AboutDialog::VersionText().startsWith(QStringLiteral("Version ")));
  CHECK(AboutDialog::LicenseText().contains(QStringLiteral("GNU GENERAL PUBLIC LICENSE")));
  CHECK(AboutDialog::LicenseText().contains(QStringLiteral("END OF TERMS AND CONDITIONS")));
  // the WinSCP icon set license (part B) is not part of NixSCP's license
  CHECK(!AboutDialog::LicenseText().contains(QStringLiteral("WinSCP Icon Set")));
  QString Clip = Dialog.ClipboardText();
  CHECK(Clip.contains(QStringLiteral("hello@codewizdev.com")));
  CHECK(Clip.contains(QStringLiteral("Dragan Plavšić")));
  CHECK(Clip.contains(QStringLiteral("Portions copyright:")));
  CHECK(Dialog.ThirdPartyBrowser()->toPlainText().contains(QStringLiteral("OpenSSL")));
  Dialog.show();
  ProcessEvents();
  Screenshot(&Dialog, QStringLiteral("about"));
  // Ctrl+C copies the whole text
  QKeyEvent CtrlC(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
  QApplication::sendEvent(&Dialog, &CtrlC);
  CHECK_EQ(QApplication::clipboard()->text(), Clip);
  // License...
  OnModal(Named(QStringLiteral("LicenseDialog")), [](QWidget * W)
  {
    CHECK_EQ(W->windowTitle(), QStringLiteral("License"));
    QPlainTextEdit * Memo = W->findChild<QPlainTextEdit *>(QStringLiteral("LicenseMemo"));
    CHECK((Memo != nullptr) && Memo->toPlainText().contains(QStringLiteral("Version 3, 29 June 2007")));
    Screenshot(W, QStringLiteral("license"));
    W->close();
  });
  Dialog.LicenseButton()->click();
  Dialog.hide();
}

//---------------------------------------------------------------------------
static void WriteBookmarks()
{
  IniFile & Ini = Configuration::Instance().Ini();
  Ini.EraseSectionTree(QStringLiteral("Configuration\\Bookmarks"));
  Bookmarks All;
  BookmarkList Site;
  Bookmark B1;
  B1.Name = QStringLiteral("Logs");
  B1.Local = QStringLiteral("/var/log");
  B1.Remote = QStringLiteral("/var/log/remote");
  Site.Add(B1);
  Bookmark B2;
  B2.Name = QStringLiteral("/srv/www");
  B2.Remote = QStringLiteral("/srv/www");
  Site.Add(B2);
  All.SetList(QStringLiteral("user@host"), Site);
  BookmarkList Shared;
  Bookmark S1;
  S1.Name = QStringLiteral("Home");
  S1.Local = QDir::homePath();
  S1.Remote = QStringLiteral("/home/user");
  S1.ShortCut = 0x4000 | '5';   // Ctrl+5
  Shared.Add(S1);
  Bookmark S2;
  S2.Name = QStringLiteral("Tmp");
  S2.Local = QStringLiteral("/tmp");
  Shared.Add(S2);
  All.SetSharedList(Shared);
  All.Save(Ini, true);
  Bookmarks::SetUseSharedBookmarks(Ini, false);
}

static QStringList ListTexts(QListWidget * List)
{
  QStringList Result;
  for (int Row = 0; Row < List->count(); Row++)
  {
    Result << List->item(Row)->text();
  }
  return Result;
}

static void TestOpenDirectory()
{
  std::printf("[open directory]\n");
  Configuration & Config = Configuration::Instance();
  IniFile & Ini = Config.Ini();
  WriteBookmarks();
  Config.UseLocationProfiles = false;

  // remote side, connected: both pages, remote directories of the bookmarks
  {
    OpenDirectoryDialog Dialog(nullptr, OpenDirectoryDialog::odBrowse, true, QStringLiteral("user@host"), true);
    CHECK_EQ(Dialog.windowTitle(), QStringLiteral("Open directory"));
  }
  QString Directory = QStringLiteral("/srv/www");
  OnModal(IsA<OpenDirectoryDialog>(), [](QWidget * W)
  {
    auto * Dialog = qobject_cast<OpenDirectoryDialog *>(W);
    Screenshot(Dialog, QStringLiteral("opendirectory"));
    CHECK(Dialog->PageControl()->isTabVisible(0));
    CHECK_EQ(Dialog->PageControl()->currentIndex(), 0);   // UseSharedBookmarks = false
    CHECK_EQ(ListTexts(Dialog->BookmarksList(false)), QStringList({ QStringLiteral("/var/log/remote"), QStringLiteral("/srv/www") }));
    // shared: Tmp has no remote directory; the shortcut is shown
    CHECK_EQ(ListTexts(Dialog->BookmarksList(true)), QStringList({ QStringLiteral("/home/user (Ctrl+5)") }));
    // the current directory is a bookmark: selected, Add disabled
    CHECK_EQ(Dialog->BookmarksList(false)->currentRow(), 1);
    CHECK(!Dialog->Button(false, QStringLiteral("Add"))->isEnabled());
    CHECK(Dialog->Button(false, QStringLiteral("Up"))->isEnabled());
    CHECK(!Dialog->Button(false, QStringLiteral("Down"))->isEnabled());
    CHECK(Dialog->Button(false, QStringLiteral("ShortCut")) == nullptr);
    CHECK(Dialog->Button(true, QStringLiteral("ShortCut")) != nullptr);
    CHECK(Dialog->SwitchButton()->isVisible());
    // Up: reorders
    Dialog->Button(false, QStringLiteral("Up"))->click();
    CHECK_EQ(ListTexts(Dialog->BookmarksList(false)).value(0), QStringLiteral("/srv/www"));
    // type a new directory and add it
    Dialog->DirectoryEdit()->setEditText(QStringLiteral("/opt/new/"));
    CHECK(Dialog->Button(false, QStringLiteral("Add"))->isEnabled());
    Dialog->BookmarksList(false)->setCurrentRow(-1);
    Dialog->DirectoryEdit()->setEditText(QStringLiteral("/opt/new/"));
    Dialog->Button(false, QStringLiteral("Add"))->click();
    CHECK_EQ(ListTexts(Dialog->BookmarksList(false)).size(), 3);
    CHECK_EQ(Dialog->Directory(), QStringLiteral("/opt/new"));
    // clicking a bookmark puts its directory into the box
    Dialog->BookmarksList(false)->setCurrentRow(1);
    CHECK_EQ(Dialog->Directory(), QStringLiteral("/var/log/remote"));
    // Remove (Delete key)
    QKeyEvent Del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QApplication::sendEvent(Dialog->BookmarksList(false), &Del);
    CHECK_EQ(ListTexts(Dialog->BookmarksList(false)), QStringList({ QStringLiteral("/srv/www"), QStringLiteral("/opt/new") }));
    // shared page
    Dialog->PageControl()->setCurrentIndex(1);
    CHECK_EQ(Dialog->Directory(), QStringLiteral("/home/user"));
    Dialog->DirectoryEdit()->setEditText(QStringLiteral("/data"));
    Dialog->accept();
  });
  bool Result = OpenDirectoryDialog::Execute(nullptr, OpenDirectoryDialog::odBrowse, true, Directory,
    { QStringLiteral("/a"), QStringLiteral("/b") }, QStringLiteral("user@host"), true);
  CHECK(Result);
  CHECK_EQ(Directory, QStringLiteral("/data"));
  // saved: order, removal, addition (Name = directory), shared page remembered
  {
    BookmarkList Site = Bookmarks::ConfiguredList(QStringLiteral("user@host"));
    CHECK_EQ(Site.Count(), 2);
    CHECK_EQ(Site.At(0).Name, QStringLiteral("/srv/www"));
    CHECK_EQ(Site.At(1).Name, QStringLiteral("/opt/new"));
    CHECK_EQ(Site.At(1).Remote, QStringLiteral("/opt/new"));
    CHECK(Site.At(1).Local.isEmpty());
    CHECK(Bookmarks::UseSharedBookmarks(Ini));
    CHECK_EQ(Bookmarks::ConfiguredSharedList().Count(), 2);
  }

  // local side, not connected: no site page, no switch, Browse button; Ctrl+B adds right away
  Directory = QStringLiteral("/usr/share");
  OnModal(IsA<OpenDirectoryDialog>(), [](QWidget * W)
  {
    auto * Dialog = qobject_cast<OpenDirectoryDialog *>(W);
    CHECK_EQ(Dialog->windowTitle(), QStringLiteral("Manage bookmarks"));
    CHECK(!Dialog->PageControl()->isTabVisible(0));
    CHECK_EQ(Dialog->PageControl()->currentIndex(), 1);
    CHECK(!Dialog->SwitchButton()->isVisible());
    CHECK(FindButton(Dialog, QStringLiteral("B&rowse...")) && FindButton(Dialog, QStringLiteral("B&rowse..."))->isVisible());
    QStringList Texts = ListTexts(Dialog->BookmarksList(true));
    CHECK(Texts.contains(QStringLiteral("/usr/share")));
    CHECK(Texts.contains(QStringLiteral("/tmp")));
    Dialog->reject();
  });
  Result = OpenDirectoryDialog::Execute(nullptr, OpenDirectoryDialog::odAddBookmark, false, Directory, {}, QString(), true);
  CHECK(!Result);
  CHECK_EQ(Directory, QStringLiteral("/usr/share"));
  // saved also on Cancel
  CHECK_EQ(Bookmarks::ConfiguredSharedList().Count(), 3);

  // "Location Profiles..." switches (UseLocationProfiles, Cancel)
  Directory = QStringLiteral("/x");
  OnModal(IsA<OpenDirectoryDialog>(), [](QWidget * W)
  {
    qobject_cast<OpenDirectoryDialog *>(W)->SwitchButton()->click();
  });
  Result = OpenDirectoryDialog::Execute(nullptr, OpenDirectoryDialog::odBrowse, true, Directory, {}, QStringLiteral("user@host"), true);
  CHECK(!Result);
  CHECK(Config.UseLocationProfiles);
  Config.UseLocationProfiles = false;

  // ---- LocationProfilesDialog::Execute dispatch (Ctrl+O)
  std::printf("[open directory dispatch]\n");
  QString Local = QStringLiteral("/home");
  QString Remote = QStringLiteral("/srv/www");
  OnModal(IsA<OpenDirectoryDialog>(), [](QWidget * W)
  {
    auto * Dialog = qobject_cast<OpenDirectoryDialog *>(W);
    CHECK_EQ(Dialog->Directory(), QStringLiteral("/srv/www"));
    Dialog->DirectoryEdit()->setEditText(QStringLiteral("/srv"));
    Dialog->accept();
  });
  Result = LocationProfilesDialog::Execute(nullptr, QStringLiteral("user@host"), true, Local, Remote, false);
  CHECK(Result);
  CHECK_EQ(Remote, QStringLiteral("/srv"));
  CHECK_EQ(Local, QStringLiteral("/home"));

  // with UseLocationProfiles: Location profiles; "Bookmarks..." goes back to Open directory
  Config.UseLocationProfiles = true;
  OnModal(IsA<LocationProfilesDialog>(), [](QWidget * W)
  {
    Screenshot(W, QStringLiteral("locationprofiles"));
    CHECK(FindButton(W, QStringLiteral("&Help")) != nullptr);
    QPushButton * Switch = W->findChild<QPushButton *>(QStringLiteral("SwitchButton"));
    CHECK(Switch != nullptr);
    if (Switch != nullptr)
    {
      OnModal(IsA<OpenDirectoryDialog>(), [](QWidget * W2)
      {
        qobject_cast<OpenDirectoryDialog *>(W2)->DirectoryEdit()->setEditText(QStringLiteral("/after/switch"));
        qobject_cast<OpenDirectoryDialog *>(W2)->accept();
      });
      Switch->click();
    }
  });
  Result = LocationProfilesDialog::Execute(nullptr, QStringLiteral("user@host"), true, Local, Remote, false);
  CHECK(Result);
  CHECK(!Config.UseLocationProfiles);
  CHECK_EQ(Remote, QStringLiteral("/after/switch"));
  // not connected: always Open directory, even with UseLocationProfiles
  Config.UseLocationProfiles = true;
  OnModal(IsA<OpenDirectoryDialog>(), [](QWidget * W) { qobject_cast<OpenDirectoryDialog *>(W)->reject(); });
  Result = LocationProfilesDialog::Execute(nullptr, QString(), false, Local, Remote, false);
  CHECK(!Result);
  Config.UseLocationProfiles = false;

  // ---- Ctrl+B: confirmation with "Add to shared bookmarks"
  std::printf("[add bookmark]\n");
  Bookmarks::SetUseSharedBookmarks(Ini, true);
  Local = QStringLiteral("/home/local");
  Remote = QStringLiteral("/remote/new");
  OnModal(IsA<MessageForm>(), [](QWidget * W)
  {
    auto * Form = qobject_cast<MessageForm *>(W);
    CHECK(Form->FormText().contains(QStringLiteral("Add directory '/remote/new' to bookmarks?")));
    CHECK(Form->NeverAskAgainCheck() != nullptr);
    if (Form->NeverAskAgainCheck() != nullptr)
    {
      CHECK_EQ(Form->NeverAskAgainCheck()->text(), QStringLiteral("Add to &shared bookmarks"));
      CHECK(Form->NeverAskAgainCheck()->isChecked());
      Form->NeverAskAgainCheck()->setChecked(false);
    }
    Screenshot(W, QStringLiteral("addbookmark"));
    OnModal(IsA<OpenDirectoryDialog>(), [](QWidget * W2)
    {
      auto * Dialog = qobject_cast<OpenDirectoryDialog *>(W2);
      // site page (shared unchecked), the directory was added and selected
      CHECK_EQ(Dialog->PageControl()->currentIndex(), 0);
      CHECK(ListTexts(Dialog->BookmarksList(false)).contains(QStringLiteral("/remote/new")));
      CHECK_EQ(Dialog->BookmarksList(false)->currentItem() ? Dialog->BookmarksList(false)->currentItem()->text() : QString(), QStringLiteral("/remote/new"));
      Dialog->accept();
    });
    Form->SubmitAnswer(qaOK);
  });
  Result = LocationProfilesDialog::Execute(nullptr, QStringLiteral("user@host"), true, Local, Remote, true);
  CHECK(Result);
  CHECK(!Bookmarks::UseSharedBookmarks(Ini));
  CHECK(Bookmarks::ConfiguredList(QStringLiteral("user@host")).IndexOf(QString(), QStringLiteral("/remote/new")) >= 0);
  // cancelled confirmation: nothing opens
  OnModal(IsA<MessageForm>(), [](QWidget * W) { qobject_cast<MessageForm *>(W)->SubmitAnswer(qaCancel); });
  Result = LocationProfilesDialog::Execute(nullptr, QStringLiteral("user@host"), true, Local, Remote, true);
  CHECK(!Result);

  // ---- shared bookmark shortcuts and OpenBookmarkTarget
  std::printf("[bookmark shortcuts]\n");
  {
    Bookmark B;
    B.Local = QStringLiteral("/l");
    B.Remote = QStringLiteral("/r");
    QString L;
    QString R;
    CHECK(Bookmarks::OpenBookmarkTarget(B, false, true, true, L, R));
    CHECK(L.isEmpty() && (R == QStringLiteral("/r")));
    CHECK(Bookmarks::OpenBookmarkTarget(B, false, true, false, L, R));
    CHECK((L == QStringLiteral("/l")) && R.isEmpty());
    CHECK(Bookmarks::OpenBookmarkTarget(B, true, true, false, L, R));
    CHECK((L == QStringLiteral("/l")) && (R == QStringLiteral("/r")));
    // location profiles need a session; not connected = local side only
    CHECK(Bookmarks::OpenBookmarkTarget(B, true, false, true, L, R));
    CHECK((L == QStringLiteral("/l")) && R.isEmpty());
    B.Remote.clear();
    CHECK(!Bookmarks::OpenBookmarkTarget(B, false, true, true, L, R));
  }
  {
    QMainWindow Window;
    QAction * Own = new QAction(QStringLiteral("Own"), &Window);
    Own->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::ALT | Qt::Key_T));
    Window.addAction(Own);
    QStringList Opened;
    LocationProfilesDialog::InstallBookmarkShortCuts(&Window, [&Opened](const Bookmark & B) { Opened << B.Name; });
    QList<QAction *> Actions = Window.findChildren<QAction *>(QStringLiteral("BookmarkShortCutAction"));
    CHECK_EQ(Actions.size(), 1);
    if (!Actions.isEmpty())
    {
      CHECK(Actions.first()->shortcut() == QKeySequence(Qt::CTRL | Qt::Key_5));
      Actions.first()->trigger();
      CHECK_EQ(Opened, QStringList({ QStringLiteral("Home") }));
    }
    // a new shortcut saved by the dialog is picked up
    BookmarkList Shared = Bookmarks::ConfiguredSharedList();
    int Index = Shared.IndexOf(QString(), QStringLiteral("Tmp"));
    CHECK(Index >= 0);
    Bookmark Tmp = Shared.At(Index);
    Tmp.ShortCut = 0x2000 | 0x4000 | 0x8000 | 'T';   // conflicts with Own: skipped
    Shared.Update(Index, Tmp);
    Bookmarks All;
    All.Load(Ini);
    All.SetSharedList(Shared);
    All.Save(Ini);
    LocationProfilesDialog::RefreshBookmarkShortCuts();
    CHECK_EQ(Window.findChildren<QAction *>(QStringLiteral("BookmarkShortCutAction")).size(), 1);
    Tmp.ShortCut = 0x4000 | '7';
    Shared.Update(Index, Tmp);
    All.SetSharedList(Shared);
    All.Save(Ini);
    LocationProfilesDialog::RefreshBookmarkShortCuts();
    CHECK_EQ(Window.findChildren<QAction *>(QStringLiteral("BookmarkShortCutAction")).size(), 2);
  }
}

//---------------------------------------------------------------------------
static SessionData SampleData()
{
  SessionData Data;
  Data.FSProtocol = fsSFTP;
  Data.HostName = QStringLiteral("example.com");
  Data.PortNumber = 2222;
  Data.UserName = QStringLiteral("martin");
  Data.Password = QStringLiteral("p@ss word");
  Data.HostKey = QStringLiteral("ssh-ed25519 255 Qrm+aUl4/zQ6UwGDCZHaHiAEGTLWHF28H5qMRRk6ag=");
  Data.RemoteDirectory = QStringLiteral("/home/martin/dir with space");
  return Data;
}

static void TestGenerateUrl()
{
  std::printf("[generate url]\n");
  SessionData Data = SampleData();
  using G = GenerateUrlDialog;
  CHECK_EQ(G::EncodeUrlString(QStringLiteral("a b@c:d/é")), QStringLiteral("a%20b%40c%3Ad%2F%C3%A9"));
  CHECK_EQ(G::EncodeUrlPath(QStringLiteral("/a b/c")), QStringLiteral("/a%20b/c"));
  CHECK_EQ(G::GenerateSessionUrl(Data, 0), QStringLiteral("sftp://example.com:2222/"));
  CHECK_EQ(G::GenerateSessionUrl(Data, G::sufUserName), QStringLiteral("sftp://martin@example.com:2222/"));
  CHECK_EQ(G::GenerateSessionUrl(Data, G::sufOpen), QStringLiteral("sftp://martin:p%40ss%20word@example.com:2222/"));
  CHECK_EQ(G::GenerateSessionUrl(Data, G::sufUserName | G::sufHostKey | G::sufSpecific),
    QStringLiteral("winscp-sftp://martin;fingerprint=ssh-ed25519-Qrm-aUl4_zQ6UwGDCZHaHiAEGTLWHF28H5qMRRk6ag@example.com:2222/"));
  // libssh notation of a host key
  SessionData LibSsh = Data;
  LibSsh.HostKey = QStringLiteral("ssh-ed25519 SHA256:Qrm+aUl4");
  CHECK_EQ(G::GenerateSessionUrl(LibSsh, G::sufUserName | G::sufHostKey), QStringLiteral("sftp://martin;fingerprint=ssh-ed25519-Qrm-aUl4@example.com:2222/"));
  // protocols
  SessionData Ftp;
  Ftp.FSProtocol = fsFTP;
  Ftp.HostName = QStringLiteral("ftp.example.com");
  Ftp.PortNumber = 21;
  CHECK_EQ(G::GenerateSessionUrl(Ftp, 0), QStringLiteral("ftp://ftp.example.com/"));
  Ftp.Ftps = ftpsExplicitTls;
  CHECK_EQ(G::GenerateSessionUrl(Ftp, 0), QStringLiteral("ftpes://ftp.example.com/"));
  Ftp.Ftps = ftpsImplicit;
  Ftp.PortNumber = 990;
  CHECK_EQ(G::GenerateSessionUrl(Ftp, 0), QStringLiteral("ftps://ftp.example.com/"));
  SessionData Dav;
  Dav.FSProtocol = fsWebDAV;
  Dav.Ftps = ftpsImplicit;
  Dav.HostName = QStringLiteral("dav.example.com");
  Dav.PortNumber = 443;
  CHECK_EQ(G::GenerateSessionUrl(Dav, 0), QStringLiteral("davs://dav.example.com/"));
  CHECK_EQ(G::GenerateSessionUrl(Dav, G::sufHttpForWebDAV), QStringLiteral("https://dav.example.com/"));
  SessionData S3;
  S3.FSProtocol = fsS3;
  S3.Ftps = ftpsNone;
  S3.HostName = QStringLiteral("s3.local");
  S3.PortNumber = 80;
  CHECK_EQ(G::GenerateSessionUrl(S3, 0), QStringLiteral("s3plain://s3.local/"));
  SessionData Ip6;
  Ip6.HostName = QStringLiteral("fe80::1");
  Ip6.PortNumber = 22;
  CHECK_EQ(G::GenerateSessionUrl(Ip6, 0), QStringLiteral("sftp://[fe80::1]/"));

  // raw settings: only non-core settings that differ from the defaults
  CHECK(G::RawSettingsForUrl(Data).isEmpty());
  SessionData Raw = Data;
  Raw.Compression = !Raw.Compression;
  Raw.Timeout = 30;
  QStringList RawSettings = G::RawSettingsForUrl(Raw);
  CHECK(RawSettings.contains(QStringLiteral("Compression=%1").arg(Raw.Compression ? 1 : 0)));
  CHECK(RawSettings.contains(QStringLiteral("Timeout=30")));
  QString RawUrl = G::GenerateSessionUrl(Raw, G::sufUserName | G::sufRawSettings);
  CHECK(RawUrl.contains(QStringLiteral(";x-timeout=30")));
  CHECK(RawUrl.contains(QStringLiteral(";x-compression=")));

  // open command arguments
  QString Open = G::GenerateOpenCommandArgs(Raw);
  CHECK(Open.startsWith(QStringLiteral("sftp://martin:p%40ss%20word@example.com:2222/ -hostkey=\"ssh-ed25519 255 Qrm+aUl4/zQ6UwGDCZHaHiAEGTLWHF28H5qMRRk6ag=\"")));
  CHECK(Open.contains(QStringLiteral(" -timeout=30")));
  CHECK(Open.contains(QStringLiteral(" -rawsettings Compression=")));
  CHECK(!Open.contains(QStringLiteral("Timeout=30")));
  SessionData Keyed = Data;
  Keyed.PublicKeyFile = QStringLiteral("/home/martin/.ssh/id \"x\"");
  CHECK(G::GenerateOpenCommandArgs(Keyed).contains(QStringLiteral(" -privatekey=\"/home/martin/.ssh/id \"\"x\"\"\"")));
  SessionData FtpActive = Ftp;
  FtpActive.FtpPasvMode = false;
  FtpActive.HostKey = QStringLiteral("aa:bb");
  QString FtpOpen = G::GenerateOpenCommandArgs(FtpActive);
  CHECK(FtpOpen.contains(QStringLiteral(" -certificate=\"aa:bb\"")));
  CHECK(FtpOpen.contains(QStringLiteral(" -passive=0")));
  SessionData SftpOnly = Data;
  SftpOnly.FSProtocol = fsSFTPonly;
  CHECK(G::GenerateOpenCommandArgs(SftpOnly).contains(QStringLiteral("-rawsettings FSProtocol=2")));

  // dialog
  Configuration::Instance().Ini().EraseSectionTree(QStringLiteral("Configuration\\Interface"));
  {
    GenerateUrlDialog Dialog(nullptr, Data);
    Dialog.LoadOptions();
    CHECK_EQ(Dialog.windowTitle(), QStringLiteral("Generate session URL/code"));
    CHECK_EQ(Dialog.PageControl()->count(), 2);   // no ".NET assembly code"
    // defaults: Username + Initial directory
    CHECK(Dialog.Check(G::gucUserName)->isChecked());
    CHECK(Dialog.Check(G::gucRemoteDirectory)->isChecked());
    CHECK(!Dialog.Check(G::gucPassword)->isChecked());
    CHECK(Dialog.Check(G::gucPassword)->isEnabled());
    CHECK(!Dialog.Check(G::gucRawSettings)->isEnabled());
    CHECK_EQ(Dialog.ResultCaption(), QStringLiteral("URL"));
    CHECK_EQ(Dialog.ResultText(), QStringLiteral("sftp://martin@example.com:2222/home/martin/dir%20with%20space/"));
    Dialog.Check(G::gucSaveExtension)->setChecked(true);
    CHECK(Dialog.ResultText().endsWith(QStringLiteral("/;save")));
    Dialog.Check(G::gucUserName)->setChecked(false);
    CHECK(!Dialog.Check(G::gucPassword)->isEnabled());
    CHECK(!Dialog.Check(G::gucHostKey)->isEnabled());
    Dialog.show();
    ProcessEvents();
    Screenshot(&Dialog, QStringLiteral("generateurl"));
    // script
    Dialog.PageControl()->setCurrentIndex(1);
    QStringList Formats;
    for (int Index = 0; Index < Dialog.ScriptFormatCombo()->count(); Index++)
    {
      Formats << Dialog.ScriptFormatCombo()->itemText(Index);
    }
    CHECK_EQ(Formats, QStringList({ QStringLiteral("Script file"), QStringLiteral("Shell script"), QStringLiteral("Command-line") }));
    QString Script = Dialog.ResultText();
    CHECK_EQ(Dialog.ResultCaption(), QStringLiteral("Script file"));
    CHECK(Script.startsWith(QStringLiteral("open sftp://martin:p%40ss%20word@example.com:2222/ -hostkey=")));
    CHECK(Script.contains(QStringLiteral("\n# Your command 1\n# Your command 2\n\nexit\n")));
    CHECK(Script.contains(QStringLiteral("/ini=nul /script=\"/path/to/script/script.txt\"")));
    Screenshot(&Dialog, QStringLiteral("generateurl-script"));
    Dialog.ScriptFormatCombo()->setCurrentIndex(G::sfShellScript);
    QString Shell = Dialog.ResultText();
    CHECK(Shell.startsWith(QStringLiteral("#!/bin/sh\n")));
    CHECK(Shell.contains(QStringLiteral("/command \\\n    \"open sftp://")));
    // shell quoting of the script quotes
    CHECK(Shell.contains(QStringLiteral("-hostkey=\\\"ssh-ed25519 255 ")));
    CHECK(Shell.contains(QStringLiteral("\"Your command 1\"")));
    CHECK(Shell.contains(QStringLiteral("exit $WINSCP_RESULT")));
    Dialog.ScriptFormatCombo()->setCurrentIndex(G::sfCommandLine);
    CHECK_EQ(Dialog.ResultCaption(), QStringLiteral("Command-line arguments"));
    CHECK(Dialog.Description().startsWith(QStringLiteral("Command-line arguments for ")));
    CHECK(Dialog.ResultText().startsWith(QStringLiteral("/log=\"/writable/path/to/log/")));
    Dialog.SaveOptions();
  }
  IniFile & Ini = Configuration::Instance().Ini();
  CHECK_EQ(Ini.ReadRaw(QStringLiteral("Configuration\\Interface"), QStringLiteral("GenerateUrlCodeTarget")), QStringLiteral("1"));
  CHECK_EQ(Ini.ReadRaw(QStringLiteral("Configuration\\Interface"), QStringLiteral("GenerateUrlScriptFormat")), QStringLiteral("2"));
  {
    GenerateUrlDialog Dialog(nullptr, Data);
    Dialog.LoadOptions();
    CHECK_EQ(Dialog.PageControl()->currentIndex(), 1);
    CHECK_EQ(Dialog.ScriptFormatCombo()->currentIndex(), 2);
    // host key unknown warning
    SessionData NoKey = Data;
    NoKey.HostKey.clear();
    GenerateUrlDialog Dialog2(nullptr, NoKey);
    Dialog2.LoadOptions();
    CHECK(Dialog2.Description().contains(QStringLiteral("A server host key is not known.")));
  }
  // WinSCP's PowerShell / assembly choices fall back
  Ini.WriteRaw(QStringLiteral("Configuration\\Interface"), QStringLiteral("GenerateUrlScriptFormat"), QStringLiteral("3"));
  Ini.WriteRaw(QStringLiteral("Configuration\\Interface"), QStringLiteral("GenerateUrlCodeTarget"), QStringLiteral("2"));
  {
    GenerateUrlDialog Dialog(nullptr, Data);
    Dialog.LoadOptions();
    CHECK_EQ(Dialog.PageControl()->currentIndex(), 0);
    CHECK_EQ(Dialog.ScriptFormatCombo()->currentIndex(), 0);
  }
  // file URLs
  {
    GenerateUrlDialog Dialog(nullptr, Data, { QStringLiteral("/home/martin/a b.txt"), QStringLiteral("rel.txt") });
    Dialog.LoadOptions();
    CHECK_EQ(Dialog.windowTitle(), QStringLiteral("Generate file URL"));
    CHECK(!Dialog.PageControl()->isTabVisible(1));
    CHECK(!Dialog.Check(G::gucRemoteDirectory)->isEnabled());
    CHECK_EQ(Dialog.ResultText(), QStringLiteral("sftp://martin@example.com:2222/home/martin/a%20b.txt\n"
      "sftp://martin@example.com:2222/home/martin/dir%20with%20space/rel.txt"));
  }
  // the Login dialog's Generate Session URL/Code is wired
  CHECK(static_cast<bool>(LoginDialog::GenerateUrlHook));
}

//---------------------------------------------------------------------------
static void TestServerInfoOffline()
{
  std::printf("[server info]\n");
  QString SftpInfo = QStringLiteral(
    "Server: SSH-2.0-OpenSSH_9.6\n"
    "SSH protocol version: 2\n"
    "SFTP protocol version: 3\n"
    "Encryption algorithm: aes256-gcm@openssh.com\n"
    "MAC algorithm: hmac-sha2-256\n"
    "Key exchange algorithm: curve25519-sha256\n"
    "Compression: No\n"
    "Host key algorithm: ssh-ed25519\n"
    "Host key fingerprint (SHA-256): SHA256:abc\n"
    "Host key fingerprint (MD5): aa:bb");
  auto Caps = [](FSCapability C) { return (C == fcModeChanging) || (C == fcAnyCommand) || (C == fcShellAnyCommand) || (C == fcSymbolicLink); };
  ServerInfoDialog Dialog(nullptr, QStringLiteral("SFTP-3"), SftpInfo, Caps);
  CHECK_EQ(Dialog.windowTitle(), QStringLiteral("Server and protocol information"));
  CHECK_EQ(ServerInfoDialog::ListValue(Dialog.ServerView(), QStringLiteral("File transfer protocol")), QStringLiteral("SFTP-3"));
  CHECK_EQ(ServerInfoDialog::ListValue(Dialog.ServerView(), QStringLiteral("Cryptographic protocol")), QStringLiteral("SSH-2"));
  CHECK_EQ(ServerInfoDialog::ListValue(Dialog.ServerView(), QStringLiteral("SSH implementation")), QStringLiteral("SSH-2.0-OpenSSH_9.6"));
  CHECK_EQ(ServerInfoDialog::ListValue(Dialog.ServerView(), QStringLiteral("Encryption algorithm")), QStringLiteral("aes256-gcm@openssh.com"));
  CHECK_EQ(ServerInfoDialog::ListValue(Dialog.ServerView(), QStringLiteral("Compression")), QStringLiteral("No"));
  CHECK(ServerInfoDialog::ListValue(Dialog.ServerView(), QStringLiteral("Remote system")).isEmpty());
  CHECK_EQ(ServerInfoDialog::ListValue(Dialog.ProtocolView(), QStringLiteral("Can change permissions")), QStringLiteral("Yes"));
  CHECK_EQ(ServerInfoDialog::ListValue(Dialog.ProtocolView(), QStringLiteral("Can execute arbitrary command")), QStringLiteral("Yes"));
  CHECK_EQ(ServerInfoDialog::ListValue(Dialog.ProtocolView(), QStringLiteral("Can create symbolic/hard link")), QStringLiteral("Yes/No"));
  CHECK_EQ(Dialog.HostKeyFingerprintSHA256(), QStringLiteral("SHA256:abc"));
  CHECK(!Dialog.HostKeyGroup()->isHidden());
  CHECK(Dialog.CertificateGroup()->isHidden());
  CHECK(Dialog.AdditionalInfo().contains(QStringLiteral("SFTP protocol version: 3")));
  CHECK(Dialog.AdditionalInfo().contains(QStringLiteral("MAC algorithm: hmac-sha2-256")));
  // no space-available callback: the page is disabled
  CHECK(!Dialog.PageControl()->isTabEnabled(2));
  QString Clip = Dialog.ClipboardText();
  CHECK(Clip.contains(QStringLiteral("File transfer protocol = SFTP-3\n")));
  CHECK(Clip.contains(QStringLiteral("Server host key fingerprints\nAlgorithm = ssh-ed25519\nSHA-256 = SHA256:abc\n")));
  CHECK(Clip.contains(QString(60, QLatin1Char('-'))));
  Dialog.show();
  ProcessEvents();
  Screenshot(&Dialog, QStringLiteral("serverinfo"));

  QString FtpInfo = QStringLiteral(
    "Server: UNIX Type: L8\n"
    "Encryption: TLS/SSL Explicit encryption, TLSv1.3\n"
    "Cipher: TLS_AES_256_GCM_SHA384\n"
    "Certificate fingerprint (SHA-256): 11:22\n"
    "Certificate fingerprint (SHA-1): 33:44\n"
    "Features: MDTM, SIZE");
  auto FtpCaps = [](FSCapability C) { return C == fcAnyCommand; };
  ServerInfoDialog FtpDialog(nullptr, QStringLiteral("FTP"), FtpInfo, FtpCaps,
    QStringLiteral("/"), [](const QString & Path, ServerInfoDialog::SpaceAvailable & Space, QString &)
    {
      Space.BytesOnDevice = (Path == QStringLiteral("/")) ? 1024 * 1024 : 0;
      return true;
    });
  CHECK_EQ(ServerInfoDialog::ListValue(FtpDialog.ServerView(), QStringLiteral("Remote system")), QStringLiteral("UNIX Type: L8"));
  CHECK_EQ(ServerInfoDialog::ListValue(FtpDialog.ServerView(), QStringLiteral("Cryptographic protocol")), QStringLiteral("TLS/SSL Explicit encryption, TLSv1.3"));
  CHECK_EQ(ServerInfoDialog::ListValue(FtpDialog.ServerView(), QStringLiteral("Encryption algorithm")), QStringLiteral("TLS_AES_256_GCM_SHA384"));
  CHECK_EQ(ServerInfoDialog::ListValue(FtpDialog.ProtocolView(), QStringLiteral("Can execute arbitrary command")), QStringLiteral("Protocol commands only"));
  CHECK_EQ(FtpDialog.CertificateFingerprintSHA256(), QStringLiteral("11:22"));
  CHECK(FtpDialog.HostKeyGroup()->isHidden());
  CHECK(!FtpDialog.CertificateGroup()->isHidden());
  CHECK(FtpDialog.PageControl()->isTabEnabled(2));
  FtpDialog.PageControl()->setCurrentIndex(2);
  CHECK(ServerInfoDialog::ListValue(FtpDialog.SpaceAvailableView(), QStringLiteral("Total bytes on device")).startsWith(QStringLiteral("1")));
  CHECK_EQ(ServerInfoDialog::ListValue(FtpDialog.SpaceAvailableView(), QStringLiteral("Free bytes on device")), QStringLiteral("Unknown"));
  FtpDialog.show();
  FtpDialog.PageControl()->setCurrentIndex(0);
  ProcessEvents();
  Screenshot(&FtpDialog, QStringLiteral("serverinfo-ftp"));
}

//---------------------------------------------------------------------------
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

static OperationResult DoOpen(Terminal & T)
{
  QEventLoop Loop;
  OperationResult Result;
  bool Called = false;
  T.Open([&](const OperationResult & R) { Result = R; Called = true; Loop.quit(); });
  if (!Called)
  {
    QTimer::singleShot(60000, &Loop, &QEventLoop::quit);
    Loop.exec();
  }
  return Result;
}

static void TestConsoleWindowSize()
{
  std::printf("[console window size]\n");
  // Execute needs a terminal; an unopened one is enough for the size handling
  TestUserInterface UI;
  SessionData Data;
  Data.HostName = QStringLiteral("127.0.0.1");
  Terminal T(Data, &UI);
  IniFile & Ini = Configuration::Instance().Ini();
  Ini.EraseSectionTree(QStringLiteral("Configuration\\Interface\\ConsoleWin"));
  QSize Shown;
  OnModal(IsA<ConsoleDialog>(), [&Shown](QWidget * W)
  {
    Shown = W->size();
    W->close();
  });
  ConsoleDialog::Execute(&T, nullptr);
  // default 570x430 at 96 DPI; not stored when unchanged
  CHECK(Shown.width() > 400);
  CHECK(!Ini.KeyExists(QStringLiteral("Configuration\\Interface\\ConsoleWin")) ||
    Ini.ReadRaw(QStringLiteral("Configuration\\Interface\\ConsoleWin"), QStringLiteral("WindowSize")).isEmpty());
  OnModal(IsA<ConsoleDialog>(), [](QWidget * W)
  {
    W->resize(700, 500);
    W->close();
  });
  ConsoleDialog::Execute(&T, nullptr);
  QString Stored = Ini.ReadRaw(QStringLiteral("Configuration\\Interface\\ConsoleWin"), QStringLiteral("WindowSize"));
  CHECK(Stored.startsWith(QStringLiteral("700,500,")));
  OnModal(IsA<ConsoleDialog>(), [&Shown](QWidget * W)
  {
    Shown = W->size();
    W->close();
  });
  ConsoleDialog::Execute(&T, nullptr);
  CHECK(Shown == QSize(700, 500));
}

static void TestConsoleFtp()
{
  QByteArray Port = qgetenv("NIXSCP_TEST_FTP_PORT");
  if (Port.isEmpty())
  {
    std::printf("[console ftp] SKIPPED (set NIXSCP_TEST_FTP_PORT)\n");
    return;
  }
  std::printf("[console ftp]\n");
  TestUserInterface UI;
  SessionData Data;
  Data.FSProtocol = fsFTP;
  Data.HostName = qEnvironmentVariable("NIXSCP_TEST_FTP_HOST", QStringLiteral("127.0.0.1"));
  Data.PortNumber = Port.toInt();
  Data.UserName = qEnvironmentVariable("NIXSCP_TEST_FTP_USER", QStringLiteral("test"));
  Data.Password = qEnvironmentVariable("NIXSCP_TEST_FTP_PASSWORD", QStringLiteral("secret"));
  Data.Timeout = 10;
  Terminal T(Data, &UI);
  OperationResult R = DoOpen(T);
  CHECK(R.Ok);
  if (!R.Ok)
  {
    std::printf("  open failed: %s\n", qPrintable(R.Message));
    return;
  }
  CHECK(T.CanAnyCommand());
  QString Output;
  OnModal(IsA<ConsoleDialog>(), [&Output](QWidget * W)
  {
    // wait for the command (PWD) to finish, then close
    auto * Poll = new QTimer(W);
    auto * Count = new int(0);
    QObject::connect(Poll, &QTimer::timeout, W, [W, Poll, Count, &Output]()
    {
      auto * Dialog = qobject_cast<ConsoleDialog *>(W);
      QPlainTextEdit * Memo = W->findChild<QPlainTextEdit *>();
      if ((!Dialog->Executing() && Memo && Memo->toPlainText().contains(QStringLiteral("257"))) || (++*Count > 500))
      {
        Poll->stop();
        delete Count;
        Output = Memo ? Memo->toPlainText() : QString();
        Screenshot(W, QStringLiteral("console-ftp"));
        W->close();
      }
    });
    Poll->start(20);
  });
  ConsoleDialog::Execute(&T, nullptr, QStringLiteral("PWD"));
  CHECK(Output.contains(QStringLiteral("$ PWD")));
  CHECK(Output.contains(QStringLiteral("257")));
  // the command went to the history
  CHECK_EQ(HistoryComboBox::History(QStringLiteral("Commands")).value(0), QStringLiteral("PWD"));

  // server information of a live FTP session
  ServerInfoDialog Info(nullptr, T.ProtocolName(), T.ServerInfo(), [&T](FSCapability C) { return T.IsCapable(C); });
  CHECK_EQ(ServerInfoDialog::ListValue(Info.ServerView(), QStringLiteral("File transfer protocol")), T.ProtocolName());
  CHECK_EQ(ServerInfoDialog::ListValue(Info.ProtocolView(), QStringLiteral("Can execute arbitrary command")), QStringLiteral("Protocol commands only"));
  T.Close();
}

static void TestSftpLive()
{
  QByteArray Port = qgetenv("NIXSCP_TEST_SSH_PORT");
  QString Key = qEnvironmentVariable("NIXSCP_TEST_SSH_KEY");
  if (Port.isEmpty() || Key.isEmpty())
  {
    std::printf("[sftp live] SKIPPED (set NIXSCP_TEST_SSH_PORT, NIXSCP_TEST_SSH_KEY)\n");
    return;
  }
  std::printf("[sftp live]\n");
  QTemporaryDir Scratch;
  TestUserInterface UI;
  SessionData Data;
  Data.FSProtocol = fsSFTP;
  Data.HostName = qEnvironmentVariable("NIXSCP_TEST_SSH_HOST", QStringLiteral("127.0.0.1"));
  Data.PortNumber = Port.toInt();
  Data.UserName = qEnvironmentVariable("NIXSCP_TEST_SSH_USER", qEnvironmentVariable("USER"));
  Data.PublicKeyFile = Key;
  Data.KnownHostsFile = Scratch.filePath(QStringLiteral("known_hosts"));
  Terminal T(Data, &UI);
  OperationResult R = DoOpen(T);
  CHECK(R.Ok);
  if (!R.Ok)
  {
    std::printf("  open failed: %s\n", qPrintable(R.Message));
    return;
  }
  ServerInfoDialog Info(nullptr, T.ProtocolName(), T.ServerInfo(), [&T](FSCapability C) { return T.IsCapable(C); });
  CHECK(ServerInfoDialog::ListValue(Info.ServerView(), QStringLiteral("SSH implementation")).startsWith(QStringLiteral("SSH-2.0-")));
  CHECK_EQ(ServerInfoDialog::ListValue(Info.ServerView(), QStringLiteral("Cryptographic protocol")), QStringLiteral("SSH-2"));
  CHECK(!ServerInfoDialog::ListValue(Info.ServerView(), QStringLiteral("Encryption algorithm")).isEmpty());
  CHECK(Info.HostKeyFingerprintSHA256().startsWith(QStringLiteral("SHA256:")));
  CHECK_EQ(ServerInfoDialog::ListValue(Info.ProtocolView(), QStringLiteral("Can change permissions")), QStringLiteral("Yes"));
  Info.show();
  ProcessEvents();
  Screenshot(&Info, QStringLiteral("serverinfo-sftp-live"));
  Info.hide();

  // generated script, run by nixscp-cli (host key from the session, as SessionDataForCode)
  QString Cli = qEnvironmentVariable("NIXSCP_TEST_CLI");
  if (Cli.isEmpty())
  {
    std::printf("  generated code run SKIPPED (set NIXSCP_TEST_CLI)\n");
  }
  else
  {
    QString Algorithm;
    for (const QString & Line : T.ServerInfo().split(QLatin1Char('\n')))
    {
      if (Line.startsWith(QStringLiteral("Host key algorithm: ")))
      {
        Algorithm = Line.mid(20);
      }
    }
    SessionData CodeData = Data;
    CodeData.KnownHostsFile.clear();
    CodeData.HostKey = Algorithm + QLatin1Char(' ') + Info.HostKeyFingerprintSHA256();
    CodeData.RemoteDirectory = QStringLiteral("/tmp");
    QString Open = GenerateUrlDialog::GenerateOpenCommandArgs(CodeData);
    QString ScriptFile = Scratch.filePath(QStringLiteral("script.txt"));
    QFile File(ScriptFile);
    CHECK(File.open(QIODevice::WriteOnly));
    File.write((QStringLiteral("open ") + Open + QStringLiteral("\ncd /tmp\npwd\nexit\n")).toUtf8());
    File.close();
    QProcess Process;
    QProcessEnvironment Env = QProcessEnvironment::systemEnvironment();
    Env.insert(QStringLiteral("XDG_CONFIG_HOME"), Scratch.path());
    Process.setProcessEnvironment(Env);
    Process.start(Cli, { QStringLiteral("/ini=nul"), QStringLiteral("/script=") + ScriptFile });
    CHECK(Process.waitForFinished(60000));
    QString Out = QString::fromUtf8(Process.readAllStandardOutput());
    CHECK_EQ(Process.exitCode(), 0);
    CHECK(Out.contains(QStringLiteral("/tmp")));
    if (Process.exitCode() != 0)
    {
      std::printf("  nixscp-cli output:\n%s\n", qPrintable(Out));
    }
    // command-line form (/command arguments) of the generated code
    QProcess Process2;
    Process2.setProcessEnvironment(Env);
    Process2.start(Cli, { QStringLiteral("/ini=nul"), QStringLiteral("/command"), QStringLiteral("open ") + Open, QStringLiteral("pwd"), QStringLiteral("exit") });
    CHECK(Process2.waitForFinished(60000));
    CHECK_EQ(Process2.exitCode(), 0);
    // URL with the host key (the WinSCP URL fingerprint notation)
    QString Url = GenerateUrlDialog::GenerateSessionUrl(CodeData, GenerateUrlDialog::sufUserName | GenerateUrlDialog::sufHostKey);
    QProcess Process3;
    Process3.setProcessEnvironment(Env);
    Process3.start(Cli, { QStringLiteral("/ini=nul"), QStringLiteral("/command"),
      QStringLiteral("open %1 -privatekey=\"%2\"").arg(Url, Key), QStringLiteral("pwd"), QStringLiteral("exit") });
    CHECK(Process3.waitForFinished(60000));
    std::printf("  URL with fingerprint (%s): nixscp-cli exit code %d\n", qPrintable(Url), Process3.exitCode());
  }
  T.Close();
}

//---------------------------------------------------------------------------
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
  Configuration::Instance().SetIniFileName(Scratch.filePath(QStringLiteral("nixscp.ini")));
  Configuration::Instance().Load();
  ScreenshotDir = qEnvironmentVariable("NIXSCP_TEST_SCREENSHOT_DIR");

  TestHistory();
  TestHelp();
  TestAbout();
  TestOpenDirectory();
  TestGenerateUrl();
  TestServerInfoOffline();
  TestConsoleWindowSize();
  TestConsoleFtp();
  TestSftpLive();

  if (Failures > 0)
  {
    std::printf("test_auxdialogs: %d FAILURE(S)\n", Failures);
    return 1;
  }
  std::printf("test_auxdialogs: OK\n");
  return 0;
}

#include "test_auxdialogs.moc"

#endif
