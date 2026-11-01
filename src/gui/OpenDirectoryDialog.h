// Port of WinSCP source/forms/OpenDirectory.{cpp,dfm} (TOpenDirectoryDialog,
// DoOpenDirectoryDialog): the single-side "Open directory" / "Manage
// bookmarks" dialog, WinSCP's default Ctrl+O / Ctrl+B dialog
// (UseLocationProfiles = false), with "Site bookmarks" and "Shared
// bookmarks" lists (Add, Remove, Up, Down, drag & drop reordering, Delete key,
// Shortcut... for shared bookmarks), the "Location Profiles..." switch and Help.
// Bookmarks are core/Bookmarks in Configuration::Instance().Ini(); changes are
// saved on close (also on Cancel), as in WinSCP.
#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

class QComboBox;
class QListWidget;
class QPushButton;
class QTabWidget;

class OpenDirectoryDialog : public QDialog
{
  Q_OBJECT

public:
  enum Mode { odBrowse, odAddBookmark };

  // DoOpenDirectoryDialog. Remote: side of the directory (osRemote/osLocal).
  // Directory: in = current directory, out = directory to open.
  // Directories: history for the directory box (visited directories).
  // SessionKey: Bookmarks::SessionKey of the connected session; empty = no
  // session (the site page is hidden). AllowSwitch: show "Location Profiles...".
  // Returns true on OK. When the user clicked "Location Profiles...",
  // UseLocationProfiles is set (and saved) and false is returned.
  static bool Execute(QWidget * Parent, Mode AMode, bool Remote, QString & Directory,
    const QStringList & Directories, const QString & SessionKey, bool AllowSwitch);

  // TCustomScpExplorerForm::DoOpenDirectoryDialog(odAddBookmark): the
  // ADD_BOOKMARK_CONFIRM prompt ("Add directory '%s' to bookmarks?" with
  // "Add to shared bookmarks", which sets UseSharedBookmarks). False = cancelled.
  static bool ConfirmAddBookmark(QWidget * Parent, const QString & Directory);

  OpenDirectoryDialog(QWidget * Parent, Mode AMode, bool Remote, const QString & SessionKey, bool AllowSwitch);
  ~OpenDirectoryDialog() override;

  QString Directory() const;
  void SetDirectory(const QString & Value);
  void SetDirectories(const QStringList & Directories);
  // Saves the bookmark lists and UseSharedBookmarks (Execute does it after the dialog closes)
  void SaveBookmarks();
  bool Switched() const;

  // For tests
  QTabWidget * PageControl() const;
  QListWidget * BookmarksList(bool Shared) const;
  QPushButton * Button(bool Shared, const QString & Name) const;   // "Add", "Remove", "Up", "Down", "ShortCut"
  QComboBox * DirectoryEdit() const;
  QPushButton * SwitchButton() const;

protected:
  void showEvent(QShowEvent * Event) override;
  bool eventFilter(QObject * Watched, QEvent * Event) override;

private:
  struct Private;
  Private * d;
};
