// Port of WinSCP source/forms/LocationProfiles.{cpp,dfm} ("Location profiles":
// Open Directory/Bookmark dialog of the Commander interface with local and
// remote directory, site and shared bookmark trees), including its
// TBookmarkNameDialog ("Add Location Profile"), TBookmarkFolderDialog
// ("Move Location Profile") and forms/Custom.cpp TShortCutDialog.
// Bookmarks are core/Bookmarks, stored in Configuration::Instance().Ini().
//
// WinSCP shortcuts: Local/Remote > Go To > Open Directory/Bookmark... Ctrl+O,
// Add Path to Bookmarks Ctrl+B (AddMode), Go to address Ctrl+L / Alt+D.
// Execute also implements TScpCommanderForm::DoOpenDirectoryDialog: it opens
// this dialog only with UseLocationProfiles (Preferences > Commander) and a
// connected session, otherwise WinSCP's default single-side Open directory
// dialog (gui/OpenDirectoryDialog), and switches between the two when the
// user clicks "Bookmarks..." / "Location Profiles...".
#pragma once

#include <QDialog>
#include <QKeySequence>
#include <QList>
#include <QStringList>

#include <functional>

class Bookmark;

class LocationProfilesDialog : public QDialog
{
  Q_OBJECT

public:
  // SessionKey: Bookmarks::SessionKey(Terminal->Data()). Remote: the active
  // panel is the remote one (focuses its directory box; names new bookmarks
  // after it). AddMode (Ctrl+B): opens "Add Location Profile" right away.
  // LocalDirectories/RemoteDirectories: history offered in the directory
  // boxes (e.g. FilePanel::VisitedDirectories()).
  // Returns true on OK; LocalDirectory/RemoteDirectory then hold the
  // directories to open (either can be empty = keep that panel; with the
  // Open directory dialog the other side keeps its input value).
  // Bookmark changes are saved (Configuration::Save) even on Cancel, as in WinSCP.
  // AddMode without location profiles first asks "Add directory '%s' to
  // bookmarks?" (with "Add to shared bookmarks").
  static bool Execute(QWidget * Parent, const QString & SessionKey, bool Remote,
    QString & LocalDirectory, QString & RemoteDirectory, bool AddMode,
    const QStringList & LocalDirectories = QStringList(), const QStringList & RemoteDirectories = QStringList());
  // The Location profiles dialog itself (no UseLocationProfiles dispatch).
  // Switched (optional): set when the user clicked "Bookmarks..." (UseLocationProfiles is then false).
  static bool ExecuteProfiles(QWidget * Parent, const QString & SessionKey, bool Remote,
    QString & LocalDirectory, QString & RemoteDirectory, bool AddMode,
    const QStringList & LocalDirectories, const QStringList & RemoteDirectories, bool * Switched = nullptr);

  // forms/Custom.cpp TShortCutDialog ("Select keyboard shortcut"): Ctrl+0..9,
  // Shift+Ctrl+Alt+A..Z not in Used (or equal to ShortCut). False = cancelled.
  static bool SelectShortCut(QWidget * Parent, int & ShortCut, const QList<int> & Used);

  // Shared bookmark shortcuts in a main window (TCustomScpExplorerForm::KeyDown
  // + TScpCommanderForm::OpenBookmark): creates a window-wide QAction on
  // Window for each shortcut of the shared bookmarks and calls Open with the
  // bookmark when pressed (see Bookmarks::OpenBookmarkTarget for the
  // directories to open). The actions are refreshed automatically whenever
  // a bookmark dialog saves bookmarks (RefreshBookmarkShortCuts); call again
  // to replace the callback. Shortcuts already taken by the window's own
  // actions are skipped.
  static void InstallBookmarkShortCuts(QWidget * Window, std::function<void(const Bookmark &)> Open);
  static void RefreshBookmarkShortCuts();

  // VCL TShortCut (as stored in WinSCP.ini) <-> Qt
  static QKeySequence ShortCutToKeySequence(int ShortCut);
  static int KeySequenceToShortCut(const QKeySequence & Sequence);
  // ShortCutToText: "Ctrl+5", "Shift+Ctrl+Alt+B"
  static QString ShortCutToText(int ShortCut);

  ~LocationProfilesDialog() override;

protected:
  void showEvent(QShowEvent * Event) override;
  void done(int Result) override;

private:
  LocationProfilesDialog(QWidget * Parent);
  struct Private;
  Private * d;
};
