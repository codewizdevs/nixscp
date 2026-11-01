// Port of WinSCP source/windows/WinHelp.cpp (TWebHelpSystem: table of
// contents and keyword help on winscp.net), windows/Tools.cpp (ShowHelp,
// FormHelp) and the Help menu actions of forms/NonVisual.{cpp,dfm}
// (TableOfContentsAction F1, HomepageAction, ForumPageAction,
// HistoryPageAction, DonatePageAction, AboutAction).
//
// Linux adaptation: the documentation, homepage, forum, history and donation
// pages are WinSCP's (NixSCP is a port; its features, captions and help
// keywords are WinSCP's). Help > Check for Updates and Help > Show Tips are
// dropped (the distribution package manager handles updates; the tips come
// from WinSCP's online update service).
#pragma once

#include <QString>
#include <QUrl>

class QAbstractButton;
class QDialogButtonBox;
class QMenu;
class QPushButton;
class QWidget;

namespace HelpSystem
{

// HOMEPAGE_URL, HISTORY_URL, FORUM_URL, DONATE_URL, DOCUMENTATION_URL2 (without ver/lang)
QUrl HomepageUrl();
// Original WinSCP project (credited in About)
QUrl WinSCPHomepageUrl();
// mailto: link to the NixSCP author
QUrl ContactUrl();
QUrl HistoryUrl();
QUrl ForumUrl();
QUrl DonateUrl();
QUrl DocumentationUrl();
// DOCUMENTATION_KEYWORD_URL2: https://winscp.net/eng/docs/<page>[#fragment];
// "task_navigate#bookmarks" keeps the fragment
QUrl KeywordUrl(const QString & Keyword);

// Opens the URL in the desktop browser (QDesktopServices::openUrl; tests
// intercept every opened URL with SetOpenUrlHandler of GuiUserInterface.h)
void OpenUrl(const QUrl & Url);
// TWebHelpSystem::ShowTableOfContents (Help > Contents)
void ShowTableOfContents();
// ShowHelp: keyword help page (OpenHelp of GuiUserInterface.h)
void ShowHelp(const QString & Keyword);

// FormHelp: F1 anywhere in Form (window shortcut) and HelpButton (if given)
// open the keyword's page. The keyword is stored as the "HelpKeyword"
// property of the form. For F1, like InvokeHelp(ActiveControl), the nearest
// "HelpKeyword" property on the focused widget's parent chain (up to the
// form) wins, so controls with their own help (Preferences search box) keep
// it; the Help button always opens the form's keyword.
void InstallFormHelp(QWidget * Form, const QString & Keyword, QAbstractButton * HelpButton = nullptr);
// Changes the form's keyword (the current page of Preferences and Advanced
// Site Settings: TPreferencesDialog::WMHelp invokes the active page's help)
void SetFormHelpKeyword(QWidget * Form, const QString & Keyword);
// The keyword F1 / Help would open now in Form (see InstallFormHelp)
QString CurrentHelpKeyword(QWidget * Form);
// Adds a "&Help" button (HelpRole) to Buttons and installs the form help
QPushButton * AddHelpButton(QDialogButtonBox * Buttons, QWidget * Form, const QString & Keyword);

// InputDialog (windows/Tools.cpp) with HelpKeyword: a text input box with
// a Help button (and F1); Ok (if given) tells whether OK was pressed
QString InputDialog(QWidget * Parent, const QString & Caption, const QString & Prompt, const QString & Value,
  const QString & Keyword, bool * Ok = nullptr);

// Help menu of the main windows (forms/ScpCommander.dfm TBXSubmenuItem22):
// &Contents (F1), -, Product &Homepage, &Support Forum, &Version History, -,
// &Donate, -, &About... (AboutDialog). Creates the actions as children of Menu.
// The F1 action uses Qt::WindowShortcut on Window (the menu's window).
void PopulateHelpMenu(QMenu * Menu, QWidget * Window);

} // namespace HelpSystem
