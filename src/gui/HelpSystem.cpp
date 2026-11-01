// Port of WinSCP source/windows/WinHelp.cpp, windows/Tools.cpp (ShowHelp,
// FormHelp) and the Help actions of forms/NonVisual.cpp
#include "gui/HelpSystem.h"
#include "gui/AboutDialog.h"
#include "gui/GuiUserInterface.h"

#include <QAbstractButton>
#include <QAction>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QMenu>
#include <QPushButton>
#include <QShortcut>
#include <QWidget>

namespace HelpSystem
{

QUrl HomepageUrl()
{
  // NixSCP homepage (author: Dragan Plavšić, CodeWiz)
  return QUrl(QStringLiteral("https://codewizdev.com/"));
}

QUrl WinSCPHomepageUrl()
{
  return QUrl(QStringLiteral("https://winscp.net/"));
}

QUrl ContactUrl()
{
  return QUrl(QStringLiteral("mailto:hello@codewizdev.com?subject=NixSCP"));
}

QUrl HistoryUrl()
{
  return QUrl(QStringLiteral("https://winscp.net/eng/docs/history"));
}

QUrl ForumUrl()
{
  return QUrl(QStringLiteral("https://winscp.net/forum/"));
}

QUrl DonateUrl()
{
  return QUrl(QStringLiteral("https://winscp.net/eng/donate.php"));
}

QUrl DocumentationUrl()
{
  return QUrl(QStringLiteral("https://winscp.net/eng/docs/"));
}

QUrl KeywordUrl(const QString & Keyword)
{
  // ShowHelp: CutToChar(HelpKeyword, '#') + the fragment appended again
  QString Page = Keyword;
  QString Fragment;
  int P = Page.indexOf(QLatin1Char('#'));
  if (P >= 0)
  {
    Fragment = Page.mid(P + 1);
    Page = Page.left(P);
  }
  QUrl Url(DocumentationUrl().toString() + Page);
  if (!Fragment.isEmpty())
  {
    Url.setFragment(Fragment);
  }
  return Url;
}

void OpenUrl(const QUrl & Url)
{
  // honours the test hook (SetOpenUrlHandler)
  OpenBrowser(Url);
}

void ShowTableOfContents()
{
  OpenUrl(DocumentationUrl());
}

void ShowHelp(const QString & Keyword)
{
  OpenHelp(Keyword);
}

QString CurrentHelpKeyword(QWidget * Form)
{
  // InvokeHelp(Form->ActiveControl): the nearest control with a keyword
  QWidget * Control = Form->focusWidget();
  while ((Control != nullptr) && (Control != Form))
  {
    QString Keyword = Control->property("HelpKeyword").toString();
    if (!Keyword.isEmpty())
    {
      return Keyword;
    }
    Control = Control->parentWidget();
  }
  return Form->property("HelpKeyword").toString();
}

// F1: the focused control's keyword (or the form's); Help button: the form's
// (the current page of Preferences / Advanced Site Settings)
static void FormHelp(QWidget * Form, bool FromControl)
{
  QString Keyword = FromControl ? CurrentHelpKeyword(Form) : Form->property("HelpKeyword").toString();
  if (Keyword.isEmpty())
  {
    // VCLCommon UseSystemSettingsPre: forms without a keyword open "start"
    ShowTableOfContents();
  }
  else
  {
    ShowHelp(Keyword);
  }
}

void InstallFormHelp(QWidget * Form, const QString & Keyword, QAbstractButton * HelpButton)
{
  Form->setProperty("HelpKeyword", Keyword);
  if (!Form->property("NixSCPFormHelpInstalled").toBool())
  {
    Form->setProperty("NixSCPFormHelpInstalled", true);
    QShortcut * F1 = new QShortcut(QKeySequence(Qt::Key_F1), Form);
    F1->setObjectName(QStringLiteral("HelpShortcut"));
    F1->setContext(Qt::WindowShortcut);
    QObject::connect(F1, &QShortcut::activated, Form, [Form]() { FormHelp(Form, true); });
  }
  if (HelpButton != nullptr)
  {
    if (HelpButton->objectName().isEmpty())
    {
      HelpButton->setObjectName(QStringLiteral("HelpButton"));
    }
    HelpButton->setEnabled(true);
    QObject::connect(HelpButton, &QAbstractButton::clicked, Form, [Form]() { FormHelp(Form, false); });
  }
}

void SetFormHelpKeyword(QWidget * Form, const QString & Keyword)
{
  Form->setProperty("HelpKeyword", Keyword);
}

QPushButton * AddHelpButton(QDialogButtonBox * Buttons, QWidget * Form, const QString & Keyword)
{
  QPushButton * HelpButton = Buttons->addButton(QStringLiteral("&Help"), QDialogButtonBox::HelpRole);
  HelpButton->setAutoDefault(false);
  InstallFormHelp(Form, Keyword, HelpButton);
  return HelpButton;
}

QString InputDialog(QWidget * Parent, const QString & Caption, const QString & Prompt, const QString & Value,
  const QString & Keyword, bool * Ok)
{
  QInputDialog Dialog(Parent);
  Dialog.setObjectName(QStringLiteral("InputDialog"));
  Dialog.setWindowTitle(Caption);
  Dialog.setLabelText(Prompt);
  Dialog.setTextValue(Value);
  // the button box exists once the layout is built (QInputDialogPrivate::ensureLayout,
  // called by sizeHint)
  (void)Dialog.sizeHint();
  QDialogButtonBox * Buttons = Dialog.findChild<QDialogButtonBox *>();
  if (Buttons != nullptr)
  {
    AddHelpButton(Buttons, &Dialog, Keyword);
  }
  else
  {
    InstallFormHelp(&Dialog, Keyword);
  }
  bool Result = (Dialog.exec() == QDialog::Accepted);
  if (Ok != nullptr)
  {
    *Ok = Result;
  }
  return Result ? Dialog.textValue() : QString();
}

void PopulateHelpMenu(QMenu * Menu, QWidget * Window)
{
  QAction * Contents = Menu->addAction(QStringLiteral("&Contents"));
  Contents->setObjectName(QStringLiteral("TableOfContentsAction"));
  Contents->setStatusTip(QStringLiteral("Opens web browser and points it to documentation table of contents"));
  Contents->setShortcut(QKeySequence(Qt::Key_F1));
  Contents->setShortcutContext(Qt::WindowShortcut);
  QObject::connect(Contents, &QAction::triggered, Menu, []() { ShowTableOfContents(); });
  if ((Window != nullptr) && !Window->actions().contains(Contents))
  {
    // the shortcut works while the menu is closed
    Window->addAction(Contents);
  }
  Menu->addSeparator();

  QAction * Homepage = Menu->addAction(QStringLiteral("Product &Homepage"));
  Homepage->setObjectName(QStringLiteral("HomepageAction"));
  Homepage->setStatusTip(QStringLiteral("Opens web browser and points it to project homepage"));
  QObject::connect(Homepage, &QAction::triggered, Menu, []() { OpenUrl(HomepageUrl()); });

  // NixSCP: feedback and bug reports go to the author instead of WinSCP's forum
  QAction * Forum = Menu->addAction(QStringLiteral("&Send Feedback / Report Bug..."));
  Forum->setObjectName(QStringLiteral("ForumPageAction"));
  Forum->setStatusTip(QStringLiteral("Opens your e-mail client to write to the NixSCP author"));
  QObject::connect(Forum, &QAction::triggered, Menu, []() { OpenUrl(ContactUrl()); });

  QAction * History = Menu->addAction(QStringLiteral("&Version History"));
  History->setObjectName(QStringLiteral("HistoryPageAction"));
  History->setStatusTip(QStringLiteral("Opens web browser and points it to application history page"));
  QObject::connect(History, &QAction::triggered, Menu, []() { OpenUrl(HistoryUrl()); });
  Menu->addSeparator();

  QAction * Donate = Menu->addAction(QStringLiteral("&Donate"));
  Donate->setObjectName(QStringLiteral("DonatePageAction"));
  Donate->setStatusTip(QStringLiteral("Opens web browser and points it to project donation page"));
  QObject::connect(Donate, &QAction::triggered, Menu, []() { OpenUrl(DonateUrl()); });
  Menu->addSeparator();

  QAction * About = Menu->addAction(QStringLiteral("&About..."));
  About->setObjectName(QStringLiteral("AboutAction"));
  About->setStatusTip(QStringLiteral("Show About box"));
  QObject::connect(About, &QAction::triggered, Menu, [Window]() { AboutDialog::Execute(Window); });
}

} // namespace HelpSystem
