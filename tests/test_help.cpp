// Help buttons and F1 of every dialog/window (gui/HelpSystem InstallFormHelp,
// FormHelp): each dialog's Help button and F1 open the WinSCP documentation
// page of the matching WinSCP form (HelpKeyword of forms/*.dfm); Preferences
// and Advanced Site Settings open the page of the CURRENT sheet; message
// boxes show Help only with a help keyword; the main window's F1 opens the
// table of contents. NIXSCP_TEST_SCREENSHOT_DIR=<dir> saves PNGs of the
// dialogs. The opened URLs are recorded through SetOpenUrlHandler
// (no browser is started). Runs offscreen.
#ifndef NIXSCP_GUI_TESTS

#include <cstdio>

int main()
{
  std::printf("SKIP: test_help needs NIXSCP_GUI_TESTS (Qt Widgets + the GUI sources)\n");
  return 0;
}

#else

#include "core/Configuration.h"
#include "core/CopyParam.h"
#include "core/CopyParamList.h"
#include "core/CustomCommands.h"
#include "core/FileSystem.h"
#include "core/RemoteFile.h"
#include "core/SessionData.h"
#include "core/Terminal.h"
#include "gui/AboutDialog.h"
#include "gui/AuthenticateDialog.h"
#include "gui/CommanderWindow.h"
#include "gui/ConsoleDialog.h"
#include "gui/CopyDialog.h"
#include "gui/CopyParamsDialog.h"
#include "gui/CustomCommandDialog.h"
#include "gui/CustomCommandRunner.h"
#include "gui/CustomCommandsPage.h"
#include "gui/EditMaskDialog.h"
#include "gui/EditorManager.h"
#include "gui/EditorWindow.h"
#include "gui/FindFilesDialog.h"
#include "gui/GenerateUrlDialog.h"
#include "gui/GuiUserInterface.h"
#include "gui/HelpSystem.h"
#include "gui/LinkDialog.h"
#include "gui/LocationProfilesDialog.h"
#include "gui/LoginDialog.h"
#include "gui/OpenDirectoryDialog.h"
#include "gui/PreferencesDialog.h"
#include "gui/ProgressDialog.h"
#include "gui/PropertiesDialog.h"
#include "gui/RemoteTransferDialog.h"
#include "gui/SaveWorkspaceDialog.h"
#include "gui/ServerInfoDialog.h"
#include "gui/SiteAdvancedDialog.h"
#include "gui/SynchronizeDialogs.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QPushButton>
#include <QRegularExpression>
#include <QShortcut>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

#include <cstdio>
#include <functional>

static int Failures = 0;
static QList<QUrl> Opened;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_MSG(Cond, Msg) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, #Cond, qPrintable(Msg)); std::fflush(stdout); Failures++; } } while (0)

static void ProcessEvents(int Ms = 20)
{
  QEventLoop Loop;
  QTimer::singleShot(Ms, &Loop, &QEventLoop::quit);
  Loop.exec();
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

static std::function<bool(QWidget *)> Titled(const QString & Title)
{
  return [Title](QWidget * W) { return W->windowTitle() == Title; };
}

static QString KeywordUrl(const QString & Keyword)
{
  // NixSCP's own pages live on nixscp.com
  if (Keyword.startsWith(QStringLiteral("nixscp:")))
  {
    return QStringLiteral("https://nixscp.com/") + Keyword.mid(7);
  }
  return QStringLiteral("https://winscp.net/eng/docs/") + Keyword;
}

static QString LastUrl()
{
  return Opened.isEmpty() ? QStringLiteral("<none>") : Opened.last().toString();
}

static bool IsHelpButton(QAbstractButton * Button)
{
  QString Text = Button->text();
  Text.remove(QLatin1Char('&'));
  return (Text == QStringLiteral("Help"));
}

// Help buttons that belong to W itself (not to a nested dialog)
static QList<QAbstractButton *> HelpButtons(QWidget * W)
{
  QList<QAbstractButton *> Result;
  for (QAbstractButton * Button : W->findChildren<QAbstractButton *>())
  {
    if (IsHelpButton(Button) && (Button->window() == W))
    {
      Result.append(Button);
    }
  }
  return Result;
}

// F1 as typed by the user: the key press goes to the focused widget of the
// active window and the shortcut map (QShortcut, Qt::WindowShortcut) handles it
static void PressF1(QWidget * W)
{
  if (!W->isVisible())
  {
    W->show();
  }
  W->activateWindow();
  W->raise();
  ProcessEvents();
  QWidget * Target = (W->focusWidget() != nullptr) ? W->focusWidget() : W;
  QKeyEvent Press(QEvent::KeyPress, Qt::Key_F1, Qt::NoModifier);
  QApplication::sendEvent(Target, &Press);
  QKeyEvent Release(QEvent::KeyRelease, Qt::Key_F1, Qt::NoModifier);
  QApplication::sendEvent(Target, &Release);
  ProcessEvents();
}

static void CheckF1(QWidget * W, const QString & Keyword, const QString & Name)
{
  Opened.clear();
  PressF1(W);
  CHECK_MSG((Opened.size() == 1) && (LastUrl() == KeywordUrl(Keyword)),
    QStringLiteral("%1 F1: %2 (expected %3)").arg(Name, LastUrl(), Keyword));
}

// The Help button(s) and F1 open Keyword's page. ExpectButton: the WinSCP
// form has a Help button (it must be there and enabled).
static void CheckHelp(QWidget * W, const QString & Keyword, const QString & Name, bool ExpectButton = true)
{
  std::printf("  %s -> %s\n", qPrintable(Name), qPrintable(Keyword));
  QString ScreenshotDir = qEnvironmentVariable("NIXSCP_TEST_SCREENSHOT_DIR");
  if (!ScreenshotDir.isEmpty())
  {
    // NIXSCP_TEST_SCREENSHOT_DIR=<dir>: PNG of each checked dialog
    QString File = Name;
    File.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9]+")), QStringLiteral("_"));
    W->grab().save(QDir(ScreenshotDir).filePath(File + QStringLiteral(".png")));
  }
  QList<QAbstractButton *> Buttons = HelpButtons(W);
  if (ExpectButton)
  {
    CHECK_MSG(!Buttons.isEmpty(), Name + QStringLiteral(": no Help button"));
  }
  else
  {
    CHECK_MSG(Buttons.isEmpty(), Name + QStringLiteral(": unexpected Help button"));
  }
  for (QAbstractButton * Button : Buttons)
  {
    CHECK_MSG(Button->isEnabled(), Name + QStringLiteral(": Help button disabled"));
    Opened.clear();
    Button->click();
    ProcessEvents(5);
    CHECK_MSG((Opened.size() == 1) && (LastUrl() == KeywordUrl(Keyword)),
      QStringLiteral("%1 Help: %2 (expected %3)").arg(Name, LastUrl(), Keyword));
  }
  CheckF1(W, Keyword, Name);
  CHECK_MSG(HelpSystem::CurrentHelpKeyword(W) == Keyword, Name);
}

// Opens a modal dialog with Run, checks its help and closes it
static void CheckModal(std::function<bool(QWidget *)> Match, const QString & Keyword, const QString & Name,
  std::function<void()> Run, bool ExpectButton = true)
{
  bool Seen = false;
  OnModal(Match, [&](QWidget * W)
  {
    Seen = true;
    CheckHelp(W, Keyword, Name, ExpectButton);
    W->close();
    if (auto * Dialog = qobject_cast<QDialog *>(W))
    {
      Dialog->reject();
    }
  });
  Run();
  CHECK_MSG(Seen, Name + QStringLiteral(" not shown"));
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

//---------------------------------------------------------------------------
static void TestHelpSystem()
{
  std::printf("[help system]\n");
  CHECK(HelpSystem::KeywordUrl(QStringLiteral("ui_login")).toString() == KeywordUrl(QStringLiteral("ui_login")));
  CHECK(HelpSystem::KeywordUrl(QStringLiteral("task_navigate#bookmarks")).toString() ==
    QStringLiteral("https://winscp.net/eng/docs/task_navigate#bookmarks"));
  // every URL opener goes through the test hook
  Opened.clear();
  HelpSystem::ShowTableOfContents();
  HelpSystem::ShowHelp(QStringLiteral("ui_copy"));
  OpenHelp(QStringLiteral("ui_pref_dragdrop#temporary"));
  CHECK(Opened.size() == 3);
  CHECK(Opened.value(0).toString() == QStringLiteral("https://winscp.net/eng/docs/"));
  CHECK(Opened.value(1).toString() == KeywordUrl(QStringLiteral("ui_copy")));
  CHECK(Opened.value(2).toString() == QStringLiteral("https://winscp.net/eng/docs/ui_pref_dragdrop#temporary"));

  // a control with its own keyword wins over the form's (InvokeHelp(ActiveControl))
  QDialog Dialog;
  auto * Edit = new QLineEdit(&Dialog);
  auto * Other = new QLineEdit(&Dialog);
  Edit->setProperty("HelpKeyword", QStringLiteral("ui_pref_search"));
  HelpSystem::InstallFormHelp(&Dialog, QStringLiteral("ui_preferences"));
  // installing twice does not duplicate F1
  HelpSystem::InstallFormHelp(&Dialog, QStringLiteral("ui_preferences"));
  CHECK(Dialog.findChildren<QShortcut *>().size() == 1);
  Dialog.show();
  Other->setFocus();
  CheckF1(&Dialog, QStringLiteral("ui_preferences"), QStringLiteral("form"));
  Edit->setFocus();
  CheckF1(&Dialog, QStringLiteral("ui_pref_search"), QStringLiteral("control"));
  HelpSystem::SetFormHelpKeyword(&Dialog, QStringLiteral("ui_pref_window"));
  Other->setFocus();
  CheckF1(&Dialog, QStringLiteral("ui_pref_window"), QStringLiteral("changed keyword"));
  Dialog.hide();

  // InputDialog with HelpKeyword
  CheckModal(IsA<QInputDialog>(), QStringLiteral("ui_tabs#renaming"), QStringLiteral("InputDialog"), []()
  {
    bool Ok = true;
    HelpSystem::InputDialog(nullptr, QStringLiteral("Rename tab"), QStringLiteral("&New tab name:"), QStringLiteral("x"),
      QStringLiteral("ui_tabs#renaming"), &Ok);
    CHECK(!Ok);
  });
}

//---------------------------------------------------------------------------
static void TestMessageBoxes()
{
  std::printf("[message boxes]\n");
  {
    MessageForm Form(nullptr, QStringLiteral("q"), QString(), qtConfirmation, qaOK | qaCancel, QStringLiteral("ui_overwrite"), nullptr);
    Form.show();
    CHECK(Form.AnswerButton(qaHelp) != nullptr);
    Opened.clear();
    Form.AnswerButton(qaHelp)->click();
    CHECK_MSG(LastUrl() == KeywordUrl(QStringLiteral("ui_overwrite")), LastUrl());
    CHECK(Form.isVisible());
    CheckF1(&Form, QStringLiteral("ui_overwrite"), QStringLiteral("MessageForm"));
  }
  {
    // no help page: no Help button, F1 does nothing
    MessageForm Form(nullptr, QStringLiteral("q"), QString(), qtInformation, qaOK, QString(), nullptr);
    Form.show();
    CHECK(Form.AnswerButton(qaHelp) == nullptr);
    CHECK(HelpButtons(&Form).isEmpty());
    Opened.clear();
    PressF1(&Form);
    CHECK(Opened.isEmpty());
  }
  {
    // MessageDialog with a fragment keyword
    CheckModal(IsA<MessageForm>(), QStringLiteral("task_navigate#bookmarks"), QStringLiteral("ConfirmAddBookmark"), []()
    {
      OpenDirectoryDialog::ConfirmAddBookmark(nullptr, QStringLiteral("/tmp"));
    });
  }
  {
    // COMPARE_NO_DIFFERENCES with HELP_COMPARE_NO_DIFFERENCES
    CheckModal(IsA<MessageForm>(), QStringLiteral("task_compare_directories"), QStringLiteral("Compare: no differences"), []()
    {
      QTemporaryDir Empty;
      RunCompareDirectories(nullptr, Empty.path(), RemoteFileList(), [](const QStringList &, const QStringList &) {});
    });
  }
}

//---------------------------------------------------------------------------
static void TestMainWindow()
{
  std::printf("[main window]\n");
  CommanderWindow Window;
  Window.show();
  Window.activateWindow();
  ProcessEvents(100);
  // F1 = Help > Contents (TableOfContentsAction)
  Opened.clear();
  PressF1(&Window);
  CHECK_MSG((Opened.size() == 1) && (LastUrl() == QStringLiteral("https://winscp.net/eng/docs/")), LastUrl());
  Window.hide();
}

//---------------------------------------------------------------------------
static void TestSimpleDialogs(const QString & Scratch)
{
  std::printf("[dialogs]\n");
  {
    AboutDialog Dialog;
    CheckHelp(&Dialog, QStringLiteral("ui_about"), QStringLiteral("About"));
  }
  {
    AuthenticateDialog Dialog(nullptr);
    Dialog.show();
    ProcessEvents();
    QAbstractButton * PasswordHelp = Dialog.findChild<QAbstractButton *>(QStringLiteral("PasswordHelpButton"));
    QAbstractButton * BannerHelp = Dialog.findChild<QAbstractButton *>(QStringLiteral("BannerHelpButton"));
    CHECK((PasswordHelp != nullptr) && (BannerHelp != nullptr));
    for (QAbstractButton * Button : {PasswordHelp, BannerHelp})
    {
      Opened.clear();
      if (Button != nullptr) Button->click();
      CHECK_MSG(LastUrl() == KeywordUrl(QStringLiteral("ui_authenticate")), LastUrl());
    }
    CheckF1(&Dialog, QStringLiteral("ui_authenticate"), QStringLiteral("Authenticate"));
    Dialog.hide();
  }
  {
    CopyDialog Dialog(true, false, { QStringLiteral("a.txt") }, QStringLiteral("/tmp"), CopyParam(), nullptr);
    CheckHelp(&Dialog, QStringLiteral("ui_copy"), QStringLiteral("Copy"));
  }
  {
    CopyParamsDialog Dialog(CopyParam(), 0, nullptr);
    CheckHelp(&Dialog, QStringLiteral("ui_transfer_custom"), QStringLiteral("CopyParamCustom"));
  }
  {
    CheckModal(IsA<CopyParamPresetDialog>(), QStringLiteral("ui_transfer_preset"), QStringLiteral("CopyParamPreset"), []()
    {
      CopyParamList List;
      int Index = -1;
      CopyParamPresetDialog::Execute(nullptr, List, Index, CopyParamPresetDialog::cpmAdd, nullptr, CopyParam());
    });
  }
  {
    CheckModal(IsA<CustomCommandDialog>(), QStringLiteral("ui_customcommand"), QStringLiteral("CustomCommand"), []()
    {
      CustomCommandType Command;
      CustomCommandList List;
      CustomCommandDialog::Execute(nullptr, Command, List, CustomCommandDialog::ccmAdd);
    });
  }
  {
    CheckModal(Titled(QStringLiteral("Custom commands")), QStringLiteral("ui_pref_commands"), QStringLiteral("Custom commands"), []()
    {
      CustomCommandsPage::Execute(nullptr);
    });
  }
  {
    CheckModal(Titled(QStringLiteral("'Cmd' command parameters")), QStringLiteral("custom_command"),
      QStringLiteral("Custom command parameters"), []()
    {
      QStringList Values;
      CustomCommandRunner::PromptValues(nullptr, QStringLiteral("Cmd"), { QString() }, { QString() }, Values);
    });
    CheckModal(Titled(QStringLiteral("Cmd - Console")), QStringLiteral("ui_console"), QStringLiteral("Custom command output"), []()
    {
      CustomCommandRunner::ShowResults(nullptr, QStringLiteral("Cmd"), { QStringLiteral("out") });
    }, false);
  }
  {
    // TSelectMaskDialog: Select / Deselect (form), Filter (HELP_FILTER), File color (HELP_FILE_COLORS)
    const struct { EditMaskDialog::Mode Mode; const char * Keyword; } Modes[] = {
      { EditMaskDialog::Select, "ui_select" }, { EditMaskDialog::Unselect, "ui_select" },
      { EditMaskDialog::Filter, "ui_filter" }, { EditMaskDialog::FileColor, "ui_file_color" } };
    for (const auto & M : Modes)
    {
      QString Keyword = QString::fromLatin1(M.Keyword);
      CheckModal(IsA<EditMaskDialog>(), Keyword, QStringLiteral("SelectMask %1").arg(static_cast<int>(M.Mode)), [&]()
      {
        QString Mask = QStringLiteral("*.txt");
        bool Apply = false;
        int Color = 0;
        if (M.Mode == EditMaskDialog::FileColor)
        {
          EditMaskDialog::ExecuteFileColor(nullptr, Mask, Color);
        }
        else
        {
          EditMaskDialog::Execute(nullptr, M.Mode, Mask, Apply);
        }
      });
    }
    CheckModal(Titled(QStringLiteral("Edit file mask")), QStringLiteral("ui_editmask"), QStringLiteral("EditMask"), []()
    {
      QString Mask = QStringLiteral("*.txt");
      EditMaskDialog::EditMask(nullptr, Mask);
    });
  }
  {
    // Edit With... (EditorPreferencesDialog)
    QMainWindow Window;
    EditorManager Manager(&Window);
    EditorManager::Hooks Hooks;
    QString File = QDir(Scratch).filePath(QStringLiteral("editwith.txt"));
    Hooks.CurrentSelection = [File]() { EditorManager::Selection S; S.Paths = { File }; return S; };
    Manager.SetHooks(Hooks);
    CheckModal(Titled(QStringLiteral("Editor")), QStringLiteral("ui_editor_preferences"), QStringLiteral("Edit With"), [&]()
    {
      Manager.ExecuteSelectionWith();
    });
  }
  {
    QString File = QDir(Scratch).filePath(QStringLiteral("edit.txt"));
    QFile F(File);
    if (F.open(QIODevice::WriteOnly)) { F.write("text\n"); F.close(); }
    auto * Editor = new EditorWindow(File, QStringLiteral("edit.txt"));
    Editor->setAttribute(Qt::WA_DeleteOnClose, false);
    Editor->show();
    ProcessEvents(50);
    CheckF1(Editor, QStringLiteral("ui_editor"), QStringLiteral("Editor"));
    // Help > Help
    QAction * HelpAction = nullptr;
    QAction * FindAction = nullptr;
    QAction * ReplaceAction = nullptr;
    for (QAction * Action : Editor->findChildren<QAction *>())
    {
      if (Action->text() == QStringLiteral("&Help")) HelpAction = Action;
      if (Action->text() == QStringLiteral("&Find...")) FindAction = Action;
      if (Action->text() == QStringLiteral("&Replace...")) ReplaceAction = Action;
    }
    CHECK(HelpAction != nullptr);
    if (HelpAction != nullptr)
    {
      Opened.clear();
      HelpAction->trigger();
      CHECK_MSG(LastUrl() == KeywordUrl(QStringLiteral("ui_editor")), LastUrl());
    }
    // Find / Replace dialogs (frShowHelp: HELP_EDITOR_FIND / HELP_EDITOR_REPLACE)
    CHECK((FindAction != nullptr) && (ReplaceAction != nullptr));
    const QPair<QAction *, QString> FindDialogs[] = {
      { FindAction, QStringLiteral("ui_editor_find") }, { ReplaceAction, QStringLiteral("ui_editor_replace") } };
    for (const auto & P : FindDialogs)
    {
      if (P.first == nullptr) continue;
      P.first->trigger();
      ProcessEvents(50);
      QDialog * Found = nullptr;
      for (QDialog * Dialog : Editor->findChildren<QDialog *>())
      {
        if (Dialog->isVisible())
        {
          Found = Dialog;
        }
      }
      CHECK_MSG(Found != nullptr, P.second);
      if (Found != nullptr)
      {
        CheckHelp(Found, P.second, Found->windowTitle());
        Found->hide();
      }
    }
    Editor->close();
    delete Editor;
  }
  {
    TestUserInterface UI;
    SessionData Data;
    Data.HostName = QStringLiteral("127.0.0.1");
    Terminal T(Data, &UI);
    {
      FindFilesDialog Dialog(&T, QStringLiteral("/"));
      CheckHelp(&Dialog, QStringLiteral("ui_find"), QStringLiteral("FileFind"));
      Dialog.hide();
    }
    CheckModal(IsA<ConsoleDialog>(), QStringLiteral("ui_console"), QStringLiteral("Console"), [&T]()
    {
      ConsoleDialog::Execute(&T, nullptr);
    });
  }
  {
    SessionData Data;
    Data.HostName = QStringLiteral("example.com");
    GenerateUrlDialog Dialog(nullptr, Data);
    CheckHelp(&Dialog, QStringLiteral("ui_generateurl"), QStringLiteral("GenerateUrl"));
  }
  {
    LinkDialog Dialog;
    CheckHelp(&Dialog, QStringLiteral("ui_symlink"), QStringLiteral("Symlink"));
  }
  {
    bool Seen = false;
    OnModal(IsA<LocationProfilesDialog>(), [&Seen](QWidget * W)
    {
      Seen = true;
      CheckHelp(W, QStringLiteral("ui_locationprofile"), QStringLiteral("LocationProfiles"));
      // TShortCutDialog with the owner's HelpKeyword
      CheckModal(Titled(QStringLiteral("Select keyboard shortcut")), QStringLiteral("ui_locationprofile"),
        QStringLiteral("ShortCut (location profiles)"), [W]()
      {
        int ShortCut = 0;
        LocationProfilesDialog::SelectShortCut(W, ShortCut, {});
      });
      qobject_cast<QDialog *>(W)->reject();
    });
    QString Local = QDir::tempPath();
    QString Remote = QStringLiteral("/home");
    LocationProfilesDialog::ExecuteProfiles(nullptr, QStringLiteral("user@host"), true, Local, Remote, false, {}, {});
    CHECK(Seen);
  }
  {
    OpenDirectoryDialog Dialog(nullptr, OpenDirectoryDialog::odBrowse, true, QStringLiteral("key"), false);
    CheckHelp(&Dialog, QStringLiteral("ui_opendir"), QStringLiteral("OpenDirectory"));
    CheckModal(Titled(QStringLiteral("Select keyboard shortcut")), QStringLiteral("ui_opendir"),
      QStringLiteral("ShortCut (open directory)"), [&Dialog]()
    {
      int ShortCut = 0;
      LocationProfilesDialog::SelectShortCut(&Dialog, ShortCut, {});
    });
    Dialog.hide();
  }
  {
    FileOperationProgress Progress;
    ProgressDialog Dialog(Progress);
    // Progress.dfm has no Help button: F1 only
    CheckHelp(&Dialog, QStringLiteral("ui_progress"), QStringLiteral("Progress"), false);
    Dialog.hide();
  }
  {
    RemoteFile File;
    File.FileName = QStringLiteral("a.txt");
    File.Permissions = 0644;
    PropertiesDialog Dialog({ File }, QStringLiteral("/home"), true);
    CheckHelp(&Dialog, QStringLiteral("ui_properties"), QStringLiteral("Properties"));
  }
  {
    CreateDirectoryDialog Dialog(true);
    CheckHelp(&Dialog, QStringLiteral("ui_create_directory"), QStringLiteral("CreateDirectory"));
  }
  {
    RemoteTransferDialog Dialog;
    CheckHelp(&Dialog, QStringLiteral("ui_duplicate"), QStringLiteral("RemoteTransfer"));
  }
  {
    RemoteMoveDialog Dialog(false, nullptr);
    CheckHelp(&Dialog, QStringLiteral("task_move_duplicate"), QStringLiteral("RemoteMove"));
  }
  {
    CheckModal(IsA<SaveWorkspaceDialog>(), QStringLiteral("ui_save_workspace"), QStringLiteral("SaveWorkspace"), []()
    {
      QString Name = QStringLiteral("ws");
      bool CreateShortcut = false;
      bool AutoSave = false;
      SaveWorkspaceDialog::Execute(nullptr, Configuration::Instance().StoredSessions(), Name, nullptr, false, CreateShortcut, AutoSave);
    });
  }
  {
    ServerInfoDialog Dialog(nullptr, QStringLiteral("SFTP-3"), QStringLiteral("info"), [](FSCapability) { return true; });
    CheckHelp(&Dialog, QStringLiteral("ui_fsinfo"), QStringLiteral("FileSystemInfo"));
  }
  {
    // Preferences > Security > master password (HELP_MASTER_PASSWORD_CHANGE)
    CheckModal(Titled(QStringLiteral("Master password")), QStringLiteral("ui_master_password_change"),
      QStringLiteral("Change master password"), []()
    {
      QString Password;
      PreferencesDialog::DoMasterPasswordDialog(nullptr, false, &Password);
    });
    CheckModal(Titled(QStringLiteral("Master password")), QStringLiteral("ui_master_password"),
      QStringLiteral("Current master password"), []()
    {
      PreferencesDialog::DoMasterPasswordDialog(nullptr, true, nullptr);
    });
  }
}

//---------------------------------------------------------------------------
static void TestLogin()
{
  std::printf("[login]\n");
  StoredSessionList & Sessions = Configuration::Instance().StoredSessions();
  {
    LoginDialog Dialog(Sessions);
    CheckHelp(&Dialog, QStringLiteral("ui_login"), QStringLiteral("Login"));
    Dialog.hide();
  }
  {
    ImportSessionsDialog Dialog(Sessions);
    CheckHelp(&Dialog, QStringLiteral("ui_import"), QStringLiteral("ImportSessions"));
    Dialog.hide();
  }
}

//---------------------------------------------------------------------------
// Expected page keywords: HelpKeyword of the TTabSheets of Preferences.dfm
static QString PreferencesKeyword(const QString & Caption)
{
  static const QHash<QString, QString> Keywords = {
    { QStringLiteral("Environment"), QStringLiteral("ui_pref_environment") },
    { QStringLiteral("Interface"), QStringLiteral("ui_pref_interface") },
    { QStringLiteral("Window"), QStringLiteral("ui_pref_window") },
    { QStringLiteral("Commander"), QStringLiteral("ui_pref_commander") },
    { QStringLiteral("Languages"), QStringLiteral("ui_preferences") },
    { QStringLiteral("Panels"), QStringLiteral("ui_pref_panels") },
    { QStringLiteral("File colors"), QStringLiteral("ui_pref_file_colors") },
    { QStringLiteral("Remote"), QStringLiteral("ui_pref_panels_remote") },
    { QStringLiteral("Local"), QStringLiteral("ui_pref_panels_local") },
    { QStringLiteral("Editors"), QStringLiteral("ui_pref_editor") },
    { QStringLiteral("Internal editor"), QStringLiteral("ui_pref_editor_internal") },
    { QStringLiteral("Transfer"), QStringLiteral("ui_pref_transfer") },
    { QStringLiteral("Drag & Drop"), QStringLiteral("ui_pref_dragdrop") },
    { QStringLiteral("Background"), QStringLiteral("ui_pref_background") },
    { QStringLiteral("Endurance"), QStringLiteral("ui_pref_resume") },
    { QStringLiteral("Network"), QStringLiteral("ui_pref_network") },
    { QStringLiteral("Security"), QStringLiteral("ui_pref_security") },
    { QStringLiteral("Logging"), QStringLiteral("ui_pref_logging") },
    { QStringLiteral("Integration"), QStringLiteral("ui_pref_integration") },
    { QStringLiteral("Applications"), QStringLiteral("ui_pref_integration_app") },
    { QStringLiteral("Commands"), QStringLiteral("ui_pref_commands") },
    { QStringLiteral("Storage"), QStringLiteral("ui_pref_storage") },
    { QStringLiteral("Updates"), QStringLiteral("nixscp:download") },
  };
  return Keywords.value(Caption);
}

static void TestPreferences()
{
  std::printf("[preferences]\n");
  PreferencesDialog Dialog;
  Dialog.show();
  ProcessEvents(50);
  QTreeWidget * Tree = Dialog.findChild<QTreeWidget *>();
  CHECK(Tree != nullptr);
  if (Tree == nullptr) return;
  int Pages = 0;
  for (QTreeWidgetItemIterator It(Tree); *It; ++It)
  {
    QString Caption = (*It)->data(0, Qt::UserRole + 1).toString();
    QString Keyword = PreferencesKeyword(Caption);
    CHECK_MSG(!Keyword.isEmpty(), QStringLiteral("unknown Preferences page ") + Caption);
    Tree->setCurrentItem(*It);
    Tree->setFocus();
    ProcessEvents(5);
    CheckHelp(&Dialog, Keyword, QStringLiteral("Preferences/") + Caption);
    Pages++;
  }
  CHECK_MSG(Pages >= 20, QString::number(Pages));
  // the search box has its own help (F1 there), the Help button keeps the page's
  QLineEdit * Search = nullptr;
  for (QLineEdit * Edit : Dialog.findChildren<QLineEdit *>())
  {
    if (Edit->placeholderText() == QStringLiteral("Search"))
    {
      Search = Edit;
    }
  }
  CHECK(Search != nullptr);
  if (Search != nullptr)
  {
    Tree->setCurrentItem(Tree->topLevelItem(0));
    Search->setFocus();
    ProcessEvents(5);
    CheckF1(&Dialog, QStringLiteral("ui_pref_search"), QStringLiteral("Preferences search box"));
    // typing a search shows the search results sheet
    Search->setText(QStringLiteral("confirm"));
    ProcessEvents(20);
    Tree->setFocus();
    CheckF1(&Dialog, QStringLiteral("ui_pref_search"), QStringLiteral("Preferences search results"));
    Search->clear();
  }
  // Execute with an initial page
  CheckModal(IsA<PreferencesDialog>(), QStringLiteral("ui_pref_editor_internal"), QStringLiteral("Preferences(Internal editor)"), []()
  {
    PreferencesDialog::Execute(nullptr, QStringLiteral("Internal editor"));
  });
  Dialog.hide();
}

//---------------------------------------------------------------------------
// HelpKeyword of the TTabSheets of SiteAdvanced.dfm
static QString SiteAdvancedKeyword(const QString & Caption)
{
  static const QHash<QString, QString> Keywords = {
    { QStringLiteral("Environment"), QStringLiteral("ui_login_environment") },
    { QStringLiteral("Directories"), QStringLiteral("ui_login_directories") },
    { QStringLiteral("Recycle bin"), QStringLiteral("ui_login_recycle_bin") },
    { QStringLiteral("SFTP"), QStringLiteral("ui_login_sftp") },
    { QStringLiteral("SCP/Shell"), QStringLiteral("ui_login_scp") },
    // ScpSheet with SFTP-only protocols (no SCP fallback)
    { QStringLiteral("Shell"), QStringLiteral("ui_login_scp") },
    { QStringLiteral("FTP"), QStringLiteral("ui_login_ftp") },
    { QStringLiteral("S3"), QStringLiteral("ui_login_s3") },
    { QStringLiteral("WebDAV"), QStringLiteral("ui_login_webdav") },
    { QStringLiteral("Connection"), QStringLiteral("ui_login_connection") },
    { QStringLiteral("Proxy"), QStringLiteral("ui_login_proxy") },
    { QStringLiteral("Tunnel"), QStringLiteral("ui_login_tunnel") },
    { QStringLiteral("TLS/SSL"), QStringLiteral("ui_login_tls") },
    { QStringLiteral("SSH"), QStringLiteral("ui_login_ssh") },
    { QStringLiteral("Key exchange"), QStringLiteral("ui_login_kex") },
    { QStringLiteral("Authentication"), QStringLiteral("ui_login_authentication") },
    { QStringLiteral("Bugs"), QStringLiteral("ui_login_bugs") },
    { QStringLiteral("Note"), QStringLiteral("ui_login_note") },
  };
  return Keywords.value(Caption);
}

static void TestSiteAdvanced()
{
  std::printf("[advanced site settings]\n");
  QSet<QString> Seen;
  for (TFSProtocol Protocol : { fsSFTP, fsSCPonly, fsFTP, fsWebDAV, fsS3 })
  {
    SessionData Data;
    Data.HostName = QStringLiteral("example.com");
    Data.FSProtocol = Protocol;
    if (Protocol == fsFTP)
    {
      Data.Ftps = ftpsExplicitTls;
    }
    SiteAdvancedDialog Dialog(Data);
    Dialog.show();
    ProcessEvents(30);
    QTreeWidget * Tree = Dialog.findChild<QTreeWidget *>();
    CHECK(Tree != nullptr);
    if (Tree == nullptr) continue;
    for (QTreeWidgetItemIterator It(Tree); *It; ++It)
    {
      QString Caption = (*It)->text(0);
      QString Keyword = SiteAdvancedKeyword(Caption);
      CHECK_MSG(!Keyword.isEmpty(), QStringLiteral("unknown Advanced Site Settings page ") + Caption);
      Tree->setCurrentItem(*It);
      Tree->setFocus();
      ProcessEvents(5);
      if (!Seen.contains(Caption))
      {
        CheckHelp(&Dialog, Keyword, QStringLiteral("SiteAdvanced/") + Caption);
        Seen.insert(Caption);
      }
      else
      {
        CHECK(HelpSystem::CurrentHelpKeyword(&Dialog) == Keyword);
      }
    }
    Dialog.hide();
  }
  CHECK_MSG(Seen.size() == 18, QStringList(Seen.values()).join(QLatin1Char(',')));
  {
    SessionData Data;
    SiteRawDialog Dialog(Data);
    CheckHelp(&Dialog, QStringLiteral("ui_login_raw"), QStringLiteral("SiteRaw"));
  }
}

//---------------------------------------------------------------------------
static bool WaitUntil(std::function<bool()> Condition, int TimeoutMs)
{
  QElapsedTimer Timer;
  Timer.start();
  while (!Condition())
  {
    if (Timer.elapsed() > TimeoutMs)
    {
      return false;
    }
    ProcessEvents(20);
  }
  return true;
}

static void TestSynchronize(const QString & Scratch)
{
  std::printf("[synchronize]\n");
  CheckModal(Titled(QStringLiteral("Synchronize")), QStringLiteral("ui_synchronize"), QStringLiteral("FullSynchronize"), []()
  {
    RunFullSynchronize(nullptr, nullptr, QStringLiteral("/a"), QStringLiteral("/b"), true, CopyParam(), {}, nullptr);
  });

  // Keep up to date and the checklist need an open session
  QByteArray PortStr = qgetenv("NIXSCP_TEST_SSH_PORT");
  if (PortStr.isEmpty() || qEnvironmentVariableIsEmpty("NIXSCP_TEST_SSH_KEY"))
  {
    std::printf("  session dialogs (keep up to date, checklist) SKIPPED (set NIXSCP_TEST_SSH_PORT, NIXSCP_TEST_SSH_KEY)\n");
    return;
  }
  SessionData Data;
  Data.HostName = qEnvironmentVariable("NIXSCP_TEST_SSH_HOST", QStringLiteral("127.0.0.1"));
  Data.PortNumber = PortStr.toInt();
  Data.UserName = qEnvironmentVariable("NIXSCP_TEST_SSH_USER", qEnvironmentVariable("USER"));
  Data.PublicKeyFile = qEnvironmentVariable("NIXSCP_TEST_SSH_KEY");
  Data.TryAgent = false;
  Data.AuthKI = false;
  Data.KnownHostsFile = QDir(Scratch).filePath(QStringLiteral("known_hosts"));
  TestUserInterface UI;
  Terminal T(Data, &UI);
  bool Called = false;
  T.Open([&](const OperationResult &) { Called = true; });
  CHECK(WaitUntil([&]() { return Called; }, 30000));
  CHECK(T.Active());
  if (!T.Active())
  {
    return;
  }
  QString Local = QDir(Scratch).filePath(QStringLiteral("sync-local"));
  QString Remote = QDir(Scratch).filePath(QStringLiteral("sync-remote"));
  QDir().mkpath(Local);
  QDir().mkpath(Remote);
  {
    QFile F(QDir(Local).filePath(QStringLiteral("new.txt")));
    if (F.open(QIODevice::WriteOnly)) { F.write("new"); F.close(); }
  }

  CheckModal(Titled(QStringLiteral("Keep remote directory up to date")), QStringLiteral("ui_keepuptodate"),
    QStringLiteral("Synchronize (keep up to date)"), [&]()
  {
    RunKeepRemoteUpToDate(nullptr, &T, Local, Remote, CopyParam(), {}, nullptr);
  });

  // Synchronize with "Preview changes" -> Synchronization checklist
  bool Finished = false;
  bool ChecklistSeen = false;
  OnModal(Titled(QStringLiteral("Synchronize")), [&](QWidget * W)
  {
    for (QAbstractButton * Button : W->findChildren<QAbstractButton *>())
    {
      if (Button->text() == QStringLiteral("&Remote")) Button->click();
      if (Button->text() == QStringLiteral("Pre&view changes")) Button->setChecked(true);
    }
    OnModal(Titled(QStringLiteral("Synchronization checklist")), [&](QWidget * Checklist)
    {
      ChecklistSeen = true;
      CheckHelp(Checklist, QStringLiteral("ui_synchronize_checklist"), QStringLiteral("SynchronizeChecklist"));
      qobject_cast<QDialog *>(Checklist)->reject();
    }, 30000);
    for (QPushButton * Button : W->findChildren<QPushButton *>())
    {
      if (Button->text() == QStringLiteral("OK"))
      {
        Button->click();
        break;
      }
    }
  });
  RunFullSynchronize(nullptr, &T, Local, Remote, true, CopyParam(), {}, [&]() { Finished = true; });
  CHECK(WaitUntil([&]() { return Finished; }, 60000));
  CHECK(ChecklistSeen);
}

//---------------------------------------------------------------------------
int main(int argc, char ** argv)
{
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
  {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  QTemporaryDir Scratch;
  qputenv("XDG_CONFIG_HOME", Scratch.path().toUtf8());
  qputenv("XDG_DATA_HOME", Scratch.filePath(QStringLiteral("data")).toUtf8());
  QApplication App(argc, argv);
  QStandardPaths::setTestModeEnabled(true);
  Configuration::Instance().SetIniFileName(Scratch.filePath(QStringLiteral("nixscp.ini")));
  Configuration::Instance().Load();
  SetOpenUrlHandler([](const QUrl & Url) { Opened.append(Url); });

  TestHelpSystem();
  TestMessageBoxes();
  TestSimpleDialogs(Scratch.path());
  TestLogin();
  TestPreferences();
  TestSiteAdvanced();
  TestSynchronize(Scratch.path());
  TestMainWindow();

  SetOpenUrlHandler(nullptr);
  if (Failures > 0)
  {
    std::printf("test_help: %d FAILURE(S)\n", Failures);
    return 1;
  }
  std::printf("test_help: OK\n");
  return 0;
}

#endif
