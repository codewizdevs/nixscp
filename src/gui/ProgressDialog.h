// Port of WinSCP source/forms/Progress.{cpp,dfm} (TProgressForm)
#pragma once

#include "gui/SystemActions.h"

#include <QDialog>

class FileOperationProgress;

// Non-modal progress window. Shown only when the operation takes over ~500 ms.
// Polls FileOperationProgress (thread-safe) every 100 ms.
//
// Toolbar (Progress.dfm): Cancel (Esc), Skip this file, Minimize (Alt+M),
// Proceed in Background (Alt+B), Once Finished (Alt+F cycles) and the
// speed limit combo (Alt+S, History\SpeedLimit, mouse wheel halves/doubles).
//
// Once it has run for the show delay (~500 ms) it holds a SleepInhibitor reference (logind sleep:idle block
// lock, TFileOperationProgressType::SystemRequired).
class ProgressDialog : public QDialog
{
  Q_OBJECT

public:
  // TCancelStatus
  enum CancelStatus { csContinue = 0, csCancelFile, csCancel, csCancelTransfer };

  ProgressDialog(FileOperationProgress & Progress, QWidget * Parent = nullptr);
  ~ProgressDialog() override;

  void Start();    // begin polling; shows itself after delay
  // Stop polling and close. Runs the Once Finished action (OnceDoneRequested,
  // Suspend / Shut Down through SystemActions) and the completion beep /
  // notification when Success and the operation was not cancelled.
  // Without the argument the operation counts as successful unless cancelled.
  void Finish();
  void Finish(bool Success);

  // Target: label (TFileOperationProgressType::Directory); hidden when empty
  void SetTarget(const QString & Directory);
  // Proceed in Background is offered when allowed explicitly or, by default,
  // when something is connected to MoveToQueueRequested at Start()
  void SetAllowMoveToQueue(bool Allow);
  // Skip this file (transfers only); default allowed
  void SetAllowSkip(bool Allow);

  SystemActions::OnceDoneOperation OnceDoneOperation() const;
  void SetOnceDoneOperation(SystemActions::OnceDoneOperation Operation);

  CancelStatus Cancel() const;
  bool MoveToQueue() const;
  // "NN% Uploading" (TProgressForm::ProgressStr); empty when not running
  QString ProgressTitle() const;
  bool MinimizedByMe() const;

  // Forces an update of the controls (normally done by the 100 ms timer)
  void UpdateControls();

signals:
  // Progress title for the main window caption (TTerminalManager::GetAppProgressTitle);
  // empty once finished. When nothing is connected, the dialog prefixes the
  // owner window's title itself ("NN% Uploading - <title>") and restores it.
  void ProgressTitleChanged(const QString & Title);
  // Proceed in Background clicked (fcMoveToQueue): the operation stops after
  // the current file; the receiver queues the remaining items.
  void MoveToQueueRequested();
  // The operation finished successfully with a Once Finished action other
  // than Stay Idle. Emitted before Sleep/Shut Down are executed, so that the
  // receiver can close the session first (TTerminal::CloseOnCompletion).
  void OnceDoneRequested(SystemActions::OnceDoneOperation Operation);

protected:
  void reject() override;   // Esc / close = Cancel
  bool eventFilter(QObject * Object, QEvent * Event) override;

private:
  struct Private;
  Private * d;

  void CancelOperation();
  void SetCancelLower(CancelStatus ACancel);
  void Minimize();
  void ItemSpeed(const QString & Text);
  void SetOwnerTitle(const QString & ProgressTitle);
};
