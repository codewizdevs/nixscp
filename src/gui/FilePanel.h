// Port of WinSCP source/components/DirView.cpp (local), UnixDirView.cpp (remote),
// packages/filemng/CustomDirView.pas, packages/my/NortonLikeListView.pas
// (Norton-like marking), packages/my/PathLabel.pas (path label) and the panel
// part of forms/ScpCommander + forms/CustomScpExplorer (path combo, history and
// navigation toolbars, status bar, incremental search, file colors).
#pragma once

#include "core/Configuration.h"
#include "core/RemoteFile.h"

#include <QPointer>
#include <QStringList>
#include <QWidget>
#include <atomic>
#include <functional>
#include <memory>

class QMimeData;
class QTreeView;
class Terminal;
class FileListModel;

// Supplies directory listings to a FilePanel
class DirectoryProvider
{
public:
  using LoadCallback = std::function<void(bool Ok, const QString & Directory, const RemoteFileList & Files,
    const QString & Error, const QString & MoreMessages)>;
  // Number of entries read so far ("%d Reading directory"); called on the GUI thread
  using ProgressCallback = std::function<void(int Count)>;

  DirectoryProvider() = default;
  DirectoryProvider(const DirectoryProvider &) {}
  DirectoryProvider & operator=(const DirectoryProvider &) { return *this; }
  virtual ~DirectoryProvider() { *FAlive = false; }
  // Lets a panel notice that its provider was destroyed before it
  std::shared_ptr<bool> AliveFlag() const { return FAlive; }

  virtual bool IsRemote() const = 0;
  // Callback may be called synchronously or later on the GUI thread
  virtual void Load(const QString & Path, LoadCallback Callback) = 0;
  // Load with progress reports; providers that cannot report progress ignore Progress
  virtual void LoadWithProgress(const QString & Path, ProgressCallback Progress, LoadCallback Callback)
  {
    Q_UNUSED(Progress);
    Load(Path, Callback);
  }
  // Stops (or abandons) the running Load; its callback may still come and is ignored
  virtual void CancelLoad() {}
  virtual QString HomeDirectory() const = 0;
  // Home Directory command (may need a round trip to the server); default HomeDirectory()
  virtual void ResolveHomeDirectory(std::function<void(const QString & Path)> Done) { Done(HomeDirectory()); }
  // The session is running another operation (automatic refreshes are skipped)
  virtual bool IsBusy() const { return false; }
  // Session user name (remote "Show inaccessible directories")
  virtual QString UserName() const { return QString(); }

private:
  std::shared_ptr<bool> FAlive = std::make_shared<bool>(true);
};

// Local listing runs on a worker thread (opendir/fstatat, cached uid/gid names),
// so slow or stale mounts cannot freeze the window (OperationWithTimeout)
class LocalDirectoryProvider : public DirectoryProvider
{
public:
  ~LocalDirectoryProvider() override;
  bool IsRemote() const override { return false; }
  void Load(const QString & Path, LoadCallback Callback) override;
  void LoadWithProgress(const QString & Path, ProgressCallback Progress, LoadCallback Callback) override;
  void CancelLoad() override;
  QString HomeDirectory() const override;
  // Hidden files are always listed (the panel filters them and counts "N hidden");
  // kept for source compatibility
  bool ShowHiddenFiles = false;
  // List synchronously on the calling thread (tests, tools)
  bool Synchronous = false;

  // Blocking listing used by Load. Cancel/Progress are optional (worker thread).
  static bool ReadDirectory(const QString & Path, QString & Canonical, RemoteFileList & Files,
    QString & Error, QString & MoreMessages, const std::atomic<bool> * Cancel = nullptr,
    const std::function<void(int Count)> & Progress = nullptr);

private:
  std::shared_ptr<std::atomic<bool>> FCancel;
};

class RemoteDirectoryProvider : public DirectoryProvider
{
public:
  explicit RemoteDirectoryProvider(Terminal * ATerminal);
  bool IsRemote() const override { return true; }
  void Load(const QString & Path, LoadCallback Callback) override;  // uses Terminal::ReadDirectory
  QString HomeDirectory() const override;                            // Terminal's initial directory
  // TUnixDirView::ExecuteHomeDirectory: site's remote directory when "Default
  // directory is home directory", the real home directory otherwise
  void ResolveHomeDirectory(std::function<void(const QString & Path)> Done) override;
  bool IsBusy() const override;
  QString UserName() const override;
  bool ShowHiddenFiles = false;
private:
  QPointer<Terminal> FTerminal;
};

class FilePanel : public QWidget
{
  Q_OBJECT

public:
  explicit FilePanel(bool Remote, QWidget * Parent = nullptr);
  ~FilePanel() override;

  bool IsRemote() const { return FRemote; }
  // nullptr = not connected: panel shows a "Not connected" placeholder with "New Session..." button
  void SetProvider(DirectoryProvider * Provider);
  DirectoryProvider * Provider() const { return FProvider; }

  QString CurrentDirectory() const;
  void ChangeDirectory(const QString & Path);   // adds to back history
  // Opens Path and focuses FocusFileName there (Find Files "Focus", bookmarks)
  void ChangeDirectory(const QString & Path, const QString & FocusFileName);
  // Focuses (and scrolls to) a file of the current directory; false if not listed
  bool FocusFile(const QString & FileName);
  // Back history + current + forward history (TCustomScpExplorerForm::CreateVisitedDirectories)
  QStringList VisitedDirectories() const;
  void Refresh();                               // reloads, keeps focus + marks by name
  // After an operation: reloads unless "Automatically refresh directory after
  // operation" (AutoReadDirectoryAfterOp) is off for a remote panel
  void RefreshAfterOperation();
  void Back();
  void Forward();
  // TCustomDirView::HistoryGo: Index < 0 goes back, > 0 forward (Back/Forward drop-downs)
  void HistoryGo(int Index);
  QString HistoryPath(int Index) const;          // 0 = current directory
  int BackCount() const;
  int ForwardCount() const;
  void ParentDirectory();                       // focuses the directory we came from
  void RootDirectory();
  void HomeDirectory();
  bool CanBack() const;
  bool CanForward() const;
  bool IsLoading() const;
  void CancelLoading();                         // "Reading directory" Cancel / Esc

  // Full paths (local: native absolute path; remote: unix path). Marked files,
  // or the focused item when nothing is marked (not with Explorer-style
  // keyboard selection, as GetMarkedFile). Never includes "..".
  QStringList SelectedPaths() const;
  QList<RemoteFile> SelectedFiles() const;
  bool HasFocusedFile() const;                  // false when ".." or nothing is focused
  RemoteFile FocusedFile() const;
  QString FullPath(const QString & FileName) const;

  // ---- Marking (TCustomNortonLikeListView) ----
  int MarkedCount() const;                      // explicitly marked files only
  QStringList MarkedNames() const;
  void SelectCurrentItem();                     // Mark > Select/Unselect (Ins)
  // VisualiseOperationFinished: unmarks a processed file and scrolls it into view
  void UnmarkFile(const QString & FileName);
  void ClearOperationSelection();               // unmarks everything after an operation
  // TCustomDirView::SetItemCalculatedSize (Calculate Directory Sizes): shows
  // Size in the Size column of a directory. Path is a full path of the current
  // directory or a bare file name; false when it is not listed here.
  bool SetItemCalculatedSize(const QString & Path, qint64 Size);
  void SaveSelectedNames();                     // before an operation (Restore Selection)
  void RestoreSelectedNames();                  // Mark > Restore Selection (Ctrl+Shift+S)
  bool SelectedNamesSaved() const;

  void SetShowHiddenFiles(bool Show);
  void SelectAll();
  void ClearSelection();
  void InvertSelection();
  // Full WinSCP file mask syntax (core/FileMasks: include|exclude, directory
  // masks, size/time conditions). ApplyToDirectories as "Apply to directories"
  // of the Select dialog (TFileFilter::Directories). Invalid masks select nothing.
  void SelectByMask(const QString & Mask, bool Select, bool ApplyToDirectories = false);
  void SelectFiles(const QStringList & Names);  // exact names, replaces marks
  // Panel filter (Local/Remote > Filter..., Ctrl+Alt+F): hides files not
  // matching the WinSCP file mask; directories stay unless excluded by a
  // directory mask. Empty = no filter. Status line shows "N filtered".
  void SetFilterMask(const QString & Mask);
  QString FilterMask() const;
  void SelectSameExtension(bool Select);
  void StartRename();                           // inline edit of focused name
  void SetActive(bool Active);                  // highlights the path label of the active panel
  bool IsActive() const { return FActive; }
  QTreeView * View() const;
  void FocusView();

  // ---- Sorting and columns ----
  // Column is FileListModel::Column. SortBy as the Sort menu actions: choosing
  // the current column again toggles the order.
  void SetSort(int Column, bool Ascending);
  void SortBy(int Column);
  int SortColumn() const;
  bool SortAscending() const;
  bool IsColumnVisible(int Column) const;
  void SetColumnVisible(int Column, bool Visible);   // shown columns move to the end
  bool IsColumnAvailable(int Column) const;          // Ext/Link target are side specific
  void AutoSizeColumns();                       // Size Automatically (Ctrl+Num+)
  void ResetColumnLayout();                     // Reset Layout
  // WinSCP Interface\Commander\LocalPanel|RemotePanel\DirViewParams string
  QString DirViewParams() const;
  void SetDirViewParams(const QString & Params);
  static QString DefaultDirViewParams(bool Remote);

  // ---- Panel parts ----
  void SetStatusBarVisible(bool Visible);       // persisted as ...Panel\StatusBar
  bool IsStatusBarVisible() const;
  void SetHistoryButtonsVisible(bool Visible);  // "History Buttons" band
  void SetNavigationButtonsVisible(bool Visible); // "Navigation Buttons" band
  // Extra toolbars: File Buttons band on a new row (NewRow), Selection Buttons
  // band next to the History/Navigation buttons (as ScpCommander.dfm DockRow)
  void AddToolbar(QWidget * Toolbar, bool NewRow = true);
  void FocusPathCombo();                        // Go To > Address bar
  void ShowPathComboPopup();                    // Change Directory (Alt+F2)
  // Re-reads panel preferences (Norton-like mode, size format, sorting, font,
  // full row select, file colors, incremental search, remote refresh...).
  // Also done by Refresh(), so Preferences take effect on the next reload.
  void ApplyConfiguration();
  // Remote drags: creates the QMimeData (e.g. with delayed text/uri-list);
  // the panel adds its own format to it
  void SetRemoteDragDataFactory(std::function<QMimeData *(const QStringList & RemotePaths)> Factory);

signals:
  void Activated();                              // panel (its view) received focus
  void DirectoryChanged(const QString & Directory);
  // Enter/double-click on a file with "Operation on double-click" Open (or when
  // nobody handles EditRequested/CopyRequested); also for "Open" generally
  void OpenFileRequested(const RemoteFile & File, const QString & FullPath);
  // Enter/double-click with "Operation on double-click" Edit (default)
  void EditRequested(const RemoteFile & File, const QString & FullPath);
  // Enter/double-click with "Operation on double-click" Copy; NoConfirmation
  // = !CopyOnDoubleClickConfirmation
  void CopyRequested(const RemoteFile & File, const QString & FullPath, bool NoConfirmation);
  void PropertiesRequested();                    // Alt+Enter, Alt+double-click
  void RenameRequested(const QString & OldPath, const QString & NewName);
  // Files dropped onto this panel. FromRemote tells where SourcePaths live.
  // TargetDirectory is the directory row dropped on, or CurrentDirectory().
  void DropRequested(const QStringList & SourcePaths, bool FromRemote, const QString & TargetDirectory, Qt::DropAction Action);
  // Remote files are being dragged; Data can be extended (WP18 drag-out)
  void DragOutRequested(const QStringList & RemotePaths, QMimeData * Data);
  // Right click, Menu key or Shift+F10. OnItem: on a file (not "..") rather than
  // on the panel background.
  void ContextMenuRequested(const QPoint & GlobalPos, bool OnItem);
  // Right click on the path label (LocalPanelPopup/RemotePanelPopup)
  void PanelMenuRequested(const QPoint & GlobalPos);
  void PathLabelDoubleClicked();                 // PathLabelDblClick (Open Directory/Bookmark)
  void NewSessionRequested();
  void LoadFailed(const QString & Error, const QString & MoreMessages);
  void SelectionChanged();
  void FilterRequested();                        // "N filtered" status panel or path label mask clicked
  // "N hidden" status panel clicked (ToggleShowHiddenFiles); when not connected
  // the panel toggles Configuration::ShowHiddenFiles of all panels itself
  void ToggleHiddenFilesRequested();
  void LoadingChanged(bool Loading);
  void ReadingProgress(int Count);               // "%d Reading directory"

private:
  bool FRemote;
  bool FActive = false;
  DirectoryProvider * FProvider = nullptr;
  // Implementation details are private to FilePanel.cpp (pimpl keeps header stable)
  struct Private;
  Private * d;
  friend struct Private;
};
