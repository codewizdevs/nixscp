// Port of WinSCP queue view: forms/CustomScpExplorer (QueuePanel: QueueLabel,
// QueueToolbar, QueueView3, QueueFileList; UpdateQueueView,
// ToggleQueueVisibility, GoToQueue, UpdateQueueLabel), the Queue actions of
// forms/NonVisual.dfm/.cpp (QueuePopup, Once Empty, speed combo) and
// windows/QueueController.cpp
#pragma once

#include <QWidget>

class QAction;
class QMenu;
class TransferQueue;

class QueuePanel : public QWidget
{
  Q_OBJECT

public:
  // Queue actions of NonVisual.dfm (Queue category)
  enum class QueueAction
  {
    Enable,               // QueueEnableAction "&Process Queue" Ctrl+Shift+Q
    GoTo,                 // QueueGoToAction "&Go To" Ctrl+Q
    ItemQuery,            // QueueItemQueryAction "&Show Query"
    ItemError,            // QueueItemErrorAction "&Show Error"
    ItemPrompt,           // QueueItemPromptAction "&Show Prompt"
    ItemExecute,          // QueueItemExecuteAction "&Execute Now"
    ItemPause,            // QueueItemPauseAction "&Suspend"
    ItemResume,           // QueueItemResumeAction "&Resume"
    ItemDelete,           // QueueItemDeleteAction "&Cancel"
    ItemSpeed,            // QueueItemSpeedAction "&Speed" (combo box)
    ItemUp,               // QueueItemUpAction "Move &Up"
    ItemDown,             // QueueItemDownAction "Move &Down"
    PauseAll,             // QueuePauseAllAction "&Suspend All"
    ResumeAll,            // QueueResumeAllAction "&Resume All"
    DeleteAll,            // QueueDeleteAllAction "&Cancel All"
    DeleteAllDone,        // QueueDeleteAllDoneAction "Delete All &Completed"
    CycleOnceEmpty,       // QueueCycleOnceEmptyAction "Once &Empty" (submenu)
    IdleOnceEmpty,        // QueueIdleOnceEmptyAction "&Stay Idle"
    DisconnectOnceEmpty,  // QueueDisconnectOnceEmptyAction2 "&Disconnect Session"
    SuspendOnceEmpty,     // QueueSuspendOnceEmptyAction2 "Slee&p Computer"
    ShutDownOnceEmpty,    // QueueShutDownOnceEmptyAction2 "&Shut Down Computer"
    ToggleShow,           // QueueToggleShowAction "&Queue"
    Show,                 // QueueShowAction "&Show"
    HideWhenEmpty,        // QueueHideWhenEmptyAction "Hide when &Empty"
    Hide,                 // QueueHideAction "&Hide"
    Toolbar,              // QueueToolbarAction "&Toolbar"
    FileList,             // QueueFileListAction "&File List"
    ResetLayoutColumns,   // QueueResetLayoutColumnsAction "&Reset Columns Layout"
    Preferences,          // QueuePreferencesAction "&Customize..."
    Count
  };

  // TOnceDoneOperation
  enum OnceEmptyOperation { odoIdle, odoDisconnect, odoSuspend, odoShutDown };

  explicit QueuePanel(QWidget * Parent = nullptr);
  ~QueuePanel() override;

  // nullptr = no session: empty, disabled
  void SetQueue(TransferQueue * Queue);
  TransferQueue * Queue() const;

  QAction * Action(QueueAction Id) const;
  // Commands > Queue submenu of the Commander window (ScpCommander.dfm QueueSubmenuItem)
  void PopulateQueueMenu(QMenu * Menu) const;

  // TCustomScpExplorerForm::UpdateQueueView: visibility by QueueView\Show
  // (Show / Hide when Empty / Hide) and the content of the queue
  bool ShouldBeVisible() const;
  // TCustomScpExplorerForm::ToggleQueueVisibility (QueueToggleShowAction)
  void ToggleVisibility();
  // TCustomScpExplorerForm::GoToQueue: focuses the queue list
  void GoTo();
  // qePendingUserAction: selects (and scrolls to) the item waiting for the
  // user; with QueueAutoPopup its query/prompt/error is shown once idle
  void ShowUserActionItem(int Id);

  OnceEmptyOperation OnceEmpty() const;
  void SetOnceEmpty(OnceEmptyOperation Operation);

  // QueueView\Height (pixels at 96 DPI), for the splitter of the main window
  static int ConfiguredHeight();

signals:
  void HideRequested();          // queue list should be hidden (QueueView\Show = Hide)
  void VisibilityChanged();      // QueueView\Show changed: re-evaluate ShouldBeVisible()
  // The queue became empty with a Once Empty action selected
  // (TCustomScpExplorerForm::UpdateQueueStatus -> Terminal->CloseOnCompletion):
  // odoDisconnect closes the session, odoSuspend/odoShutDown also suspend or
  // power off the computer. Without a receiver, suspend and shut down are
  // done through systemd-logind (systemctl).
  void OnceEmptyRequested(int Operation);
  // Preferences were changed through "Customize..."
  void ConfigurationChanged();

protected:
  bool event(QEvent * Event) override;
  bool eventFilter(QObject * Object, QEvent * Event) override;
  void resizeEvent(QResizeEvent * Event) override;

private:
  struct Private;
  Private * d;
};
