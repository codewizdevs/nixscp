// WinSCP icon set: port of source/forms/Glyphs.{cpp,dfm} (TGlyphsModule image
// lists, scaled variants Glyphs120/144/192), the image indexes of the actions in
// source/forms/NonVisual.dfm, LoadDialogImage and TFrameAnimation from
// source/windows/GUITools.cpp (images in source/forms/Animations*.dfm) and the
// application icon source/resource/Application.ico.
// The PNGs are extracted by tools/extract_glyphs.py into resources/.
// The images are not GPL, see resources/icons/README (License).
#pragma once

#include <QIcon>
#include <QList>
#include <QPixmap>
#include <QString>
#include <QWidget>

class QTimer;

class Icons
{
public:
  // Multi-size icon :/icons/<size>/<Name>.png: WinSCP glyphs in 16/20/24/32 px
  // (Glyphs.dfm for 96/120/144/192 DPI), app and site icons up to 256 px.
  // QIcon picks the size by the widget's icon size and device pixel ratio.
  // Without the resource (default build, -DNIXSCP_WINSCP_ICONS=OFF) it falls
  // back to the desktop icon theme: a chain of freedesktop names per icon
  // (Icon Naming Specification plus common Breeze/Adwaita/Yaru names, each
  // also tried as <name>-symbolic, drawn in the palette's text color as GTK
  // does), then a QStyle standard icon. Every name below has a fallback
  // except session-tab-color-mask; all resolve on Breeze, Adwaita, Yaru and
  // elementary-xfce (tests/test_verify_desktop.cpp). If Qt runs without a
  // platform theme (offscreen, bare window managers, KDE without the Plasma
  // integration), the XDG icon directories and the desktop's icon theme
  // (GTK settings.ini, Xfce xsettings, kdeglobals; else breeze/Papirus/Yaru/
  // elementary-xfce/Adwaita...) are used.
  // A name without any icon returns a null icon (and warns once). Results are
  // cached; call from the GUI thread only (needs a QGuiApplication).
  //
  // Names (#WinSCP image index, WinSCP image name):
  // BEGIN GENERATED ICON NAMES
  //   about                        #65  "About"
  //   back                         #6   "Back"
  //   bookmark                     #54  "Add Path to Bookmarks"
  //   change-path-local            #30  "Change Drive"
  //   change-path-remote           #29  "Change Directory"
  //   check-updates                #63  "Check for Updates"
  //   check-updates-disabled       #81  Check for updates - automatic disabled
  //   check-updates-results        #80  Check for updates - display results
  //   clear-selection              #23  "Clear Selection"
  //   close-tab                    #26  "Close Tab"
  //   column-changed               #48  "Date modified"
  //   column-ext                   #45  "Extension"
  //   column-group                 #51  "Group"
  //   column-link-target           #82  "Link Target"
  //   column-name                  #44  "Name"
  //   column-owner                 #50  "Owner"
  //   column-rights                #49  "Permissions", "Attributes"
  //   column-size                  #47  "Size"
  //   column-type                  #46  "Type"
  //   command                      #90  "Enter"
  //   compare                      #52  "Compare Directories"
  //   console                      #55  "Open Terminal"
  //   copy                         #111 "Copy to Clipboard"
  //   delete                       #2   "Delete"
  //   disconnect                   #116 "Disconnect Session"
  //   download                     #89  "Download"
  //   download-move                #97  "Download and Delete"
  //   download-queue               #107 "Download in Background"
  //   duplicate                    #78  "Duplicate", "Copy"
  //   duplicate-r-to-l             #112 "Copy"
  //   duplicate-session            #91  "Duplicate Tab"
  //   edit                         #57  "Edit"
  //   explore                      #56  "Explore Directory"
  //   filter                       #92  "Filter"
  //   find-files                   #95  "Find Files"
  //   forward                      #7   "Forward"
  //   help                         #79  "Contents"
  //   home-dir                     #15  "Home Directory"
  //   homepage                     #42  "Product Homepage"
  //   invert-selection             #22  "Invert Selection"
  //   keep-up-to-date              #53  "Keep Remote Directory up to Date"
  //   link                         #60  "Edit Link", "Link", "Shortcut"
  //   local-browser                #114 Local File Manager Tab
  //   move                         #100 "Move To", "Move"
  //   move-r-to-l                  #113 "Move"
  //   new-file                     #77  "File"
  //   new-folder                   #5   "Create Directory", "Directory"
  //   new-local-tab                #115 "New Tab", "Local Tab"
  //   new-session                  #25  "Remote Tab"
  //   open                         #58  "Open"
  //   open-dir                     #18  "Open Directory/Bookmark"
  //   opened-tabs                  #62  "Opened Tabs"
  //   parent-dir                   #12  "Parent Directory"
  //   paste                        #75  "Paste from Clipboard"
  //   preferences                  #28  "Preferences", "Customize", "Configure"
  //   properties                   #4   "Properties"
  //   putty                        #64  "Open in PuTTY"
  //   queue                        #74  "Go To", "Queue"
  //   queue-delete                 #71  "Cancel"
  //   queue-delete-all             #106 "Cancel All"
  //   queue-delete-all-done        #99  "Delete All Completed"
  //   queue-disconnect-once-empty  #87  "Disconnect Session"
  //   queue-down                   #73  "Move Down"
  //   queue-enable                 #96  "Process Queue"
  //   queue-error                  #68  "Show Error"
  //   queue-execute                #70  "Execute Now", "Resume"
  //   queue-idle-once-empty        #94  "Stay Idle", "Once Empty"
  //   queue-item-delete            #127 Delete file
  //   queue-item-download          #119 Download File
  //   queue-item-download-move     #120 Download File Remove Original
  //   queue-item-error             #124 Operation has some error
  //   queue-item-paused            #126 Suspend background-queued operation
  //   queue-item-prompt            #125 Operation needs user interaction 2
  //   queue-item-query             #123 Operation needs user interaction
  //   queue-item-upload            #121 Upload File
  //   queue-item-upload-move       #122 Upload File Remove Original
  //   queue-pause                  #83  "Suspend"
  //   queue-pause-all              #84  "Suspend All"
  //   queue-prompt                 #69  "Show Prompt"
  //   queue-query                  #67  "Show Query"
  //   queue-resume-all             #85  "Resume All"
  //   queue-shutdown-once-empty    #93  "Shut Down Computer"
  //   queue-suspend-once-empty     #105 "Sleep Computer"
  //   queue-up                     #72  "Move Up"
  //   quit                         #61  "Quit"
  //   refresh                      #16  "Refresh"
  //   rename                       #3   "Rename"
  //   restore-selection            #86  "Restore Selection"
  //   root-dir                     #13  "Root Directory"
  //   root-dir-local               #14  "Root Directory"
  //   save-session                 #43  "Save Session as Site"
  //   save-workspace               #102 "Save Workspace"
  //   select                       #19  "Select Files"
  //   select-all                   #21  "Select All"
  //   server-info                  #17  "Server/Protocol Information"
  //   session-encrypted            #118 Encrypted connection status bar indicator
  //   session-tab                  #103 Site
  //   session-tab-color-mask       #104 Site color mask
  //   sites                        #27  "Sites"
  //   sort-ascending               #41  "Sort Ascending"
  //   sort-changed                 #33  "By Date modified"
  //   sort-descending              #40  "Sort Descending"
  //   sort-ext                     #32  "By Extension"
  //   sort-group                   #39  "By Group"
  //   sort-name                    #31  "By Name"
  //   sort-owner                   #38  "By Owner"
  //   sort-rights                  #36  "By Attributes", "By Permissions/Attributes"
  //   sort-size                    #35  "By Size"
  //   sort-toggle                  #37  "Ascending"
  //   sort-type                    #34  "By Type"
  //   speed-limit                  #109 Speed Limit
  //   synchronize                  #66  "Synchronize"
  //   synchronize-browsing         #59  "Synchronize Browsing"
  //   tips                         #110 "Show Tips"
  //   tree                         #76  "Go to Tree", "Tree"
  //   unselect                     #20  "Unselect Files"
  //   upload                       #88  "Upload"
  //   upload-move                  #98  "Upload and Delete"
  //   upload-queue                 #108 "Upload in Background"
  //   view-details                 #11  "Details"
  //   view-large-icons             #8   "View", "Large Icons"
  //   view-list                    #10  "List"
  //   view-small-icons             #9   "Small Icons"
  //   workspaces                   #101 "Workspaces"
  //   nixscp                            Application icon (Application.ico), 16/20/24/32/40/42/48/60/64/128/256 px
  //   site                              Site (Site.ico), 16/32/48/60 px
  //   site-folder                       Site folder (SiteFolder.ico), 16/32/48/60 px
  //   workspace                         Workspace (Workspace.ico), 16/32/48/60 px
  // END GENERATED ICON NAMES
  static QIcon Get(const QString & Name);

  // Icon name of a WinSCP action (TNonVisualDataModule action name from
  // NonVisual.dfm, e.g. "RemoteCopyAction" -> "download"). Trailing version
  // digits are ignored ("LocalDeleteAction" matches "LocalDeleteAction2").
  // Empty when the action has no glyph in WinSCP.
  static QString ActionIconName(const QString & ActionName);
  static QIcon ForAction(const QString & ActionName);

  // The first of the freedesktop icon Names (space separated, best first, each
  // also tried as <name>-symbolic) that the desktop icon theme has; null if
  // none. Unlike QIcon::fromTheme(Name, Fallback) chains it does not pay for
  // misses (3-10 ms each where a theme folder has no icon-theme.cache, see
  // ThemeIconIndex in Icons.cpp). Cached; GUI thread only.
  static QIcon FromTheme(const QString & Names);

  // Desktop compatibility checks (tests/test_verify_desktop.cpp): the names
  // of the theme fallback tables of Get and DialogImage (slugs), and the
  // freedesktop icon name the current icon theme resolves one of them to
  // (empty: no theme icon, Get uses the QStyle icon or none)
  static QStringList FallbackNames();
  static QStringList FallbackDialogImageNames();
  static QString FallbackThemeIconName(const QString & Name, bool DialogImage = false);

  // Application icon (Application.ico, 16-256 px)
  static QIcon Application();

  // LoadDialogImage: dialog images by their WinSCP name ("Error", "Warning",
  // "Information", "Help Blue", "Open folder", "Synchronize directories",
  // "Keep remote directory up to date", "Open console window",
  // "Duplicate L to R", "Commander", "Explorer", ...), 32/40/48/64 px.
  // Without the resource: a theme or QStyle icon, null for "Commander" and
  // "Explorer" (interface previews).
  static QIcon DialogImage(const QString & Name);

  struct AnimationFrame
  {
    QPixmap Pixmap;
    int Index;        // <= 0: intro frames, > 0: looped frames
    int Duration;     // milliseconds
  };
  // Frames of a TFrameAnimation ("CopyRemote", "CopyLocal", "MoveRemote",
  // "MoveLocal", "Delete", "Recycle", "CalculateSize", "SetProperties",
  // "SynchronizeDirectories", "Find", "DuplicateLtoR", "MoveLtoR",
  // "ConnectingSecure", "ConnectingInsecure"); Size is 32, 40, 48 or 64.
  static QList<AnimationFrame> AnimationFrames(const QString & Name, int Size = 32);
};

// TFrameAnimation: 32x32 (logical) widget playing one of the WinSCP animations.
// Picks the 40/48/64 px frames on high-DPI screens.
class FrameAnimation : public QWidget
{
public:
  explicit FrameAnimation(QWidget * Parent = nullptr);

  void Init(const QString & Name);
  void Start();
  void Stop();

  QSize sizeHint() const override;

protected:
  void paintEvent(QPaintEvent * Event) override;

private:
  void Animate();

  QString FName;
  QList<Icons::AnimationFrame> FFrames;
  int FFirstLoopFrame;
  int FCurrentFrame;
  QTimer * FTimer;
};
