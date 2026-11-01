// Port of WinSCP source/core/Queue.{h,cpp} (TTerminalQueue, TTerminalItem,
// TTransferQueueItem, TParallelTransferQueueItem, TRemoteDeleteQueueItem,
// TLocalDeleteQueueItem) and the parallel transfer part of
// source/core/Terminal.cpp (TParallelOperation, CopyToParallel,
// CheckParallelFileTransfer).
//
// Background transfers over their own connections (as WinSCP: up to
// TransfersLimit parallel connections per session, opened lazily, reused).
// One transfer may use several connections ("Use multiple connections for
// single transfer", CopyParam::QueueParallel), a single large file is then
// downloaded in segments (Configuration ParallelTransferThreshold).
// Queries, prompts and errors of the background connections do not pop up:
// the item waits in qsQuery/qsPrompt/qsError state until ProcessUserAction
// (Show Query / Show Prompt / Show Error, or QueueAutoPopup).
#pragma once

#include "core/CopyParam.h"
#include "core/FileSystem.h"
#include "core/SessionData.h"
#include "core/Terminal.h"

#include <QObject>
#include <QList>
#include <QPair>

struct QueueItemInfo
{
  // TQueueItem::TStatus (qsQuery/qsError/qsPrompt are the user action states)
  enum Status { qsPending, qsConnecting, qsProcessing, qsPaused, qsDone, qsError, qsQuery, qsPrompt };
  // TFileOperation of the item (foCopy/foMove are Transfer + Move, foDelete is Delete)
  enum Kind { ikTransfer, ikDelete };

  int Id = 0;
  Status State = qsPending;
  Kind Type = ikTransfer;
  bool ToRemote = true;          // Upload / Download; ikDelete: remote (true) or local files
  bool Move = false;
  QStringList Sources;
  QString TargetDirectory;
  FileOperationProgress::Snapshot Progress{};   // valid once started (processing/paused/done)
  QString Error;                 // qsError: message
  QString MoreMessages;
  // Finished: done, or failed for good (qsError waiting to be acknowledged by Show Error)
  bool Finished = false;
  bool Listing = false;          // collecting files of a parallel transfer ("Listing...")
  int Connections = 0;           // connections working on the item (> 1: parallel transfer)
  // Parallel transfer: progress of the file (or segment) of each connection
  // (TParallelTransferQueueItem lines)
  QList<FileOperationProgress::Snapshot> ConnectionProgress;
  unsigned long CPSLimit = 0;    // speed limit of the item (bytes/s, 0 = unlimited)
  QString UserActionText;        // pending query/prompt (qsQuery/qsPrompt), tooltip

  static bool IsUserActionStatus(Status S) { return (S == qsQuery) || (S == qsError) || (S == qsPrompt); }
};

// TQueueFileState
enum QueueFileState { qfsQueued = 0, qfsProcessed = 1 };

class TransferQueue : public QObject
{
  Q_OBJECT

public:
  // Data must contain everything to log in without prompting where possible
  // (password typed in the main session is passed on, as WinSCP does).
  // UI is the interactive user interface (GUI thread capable); queries of the
  // background connections are passed to it by ProcessUserAction.
  // Options default to Configuration::Instance() (QueueTransfersLimit,
  // EnableQueueByDefault, QueueKeepDoneItems/For, ParallelTransferThreshold,
  // SessionReopenBackground, ParallelDurationThreshold); options not set
  // explicitly follow later changes of the configuration.
  TransferQueue(const SessionData & Data, UserInterface * UI, QObject * Parent = nullptr);
  ~TransferQueue() override;

  // CopyParam.QueueNoConfirmation turns off overwrite/resume confirmations
  // (cpNoConfirmation), CopyParam.QueueParallel allows multiple connections
  int AddUpload(const QStringList & LocalPaths, const QString & RemoteDirectory, const CopyParam & Param, bool Move);
  int AddDownload(const QStringList & RemotePaths, const QString & LocalDirectory, const CopyParam & Param, bool Move);
  // TRemoteDeleteQueueItem / TLocalDeleteQueueItem; local files go to the
  // desktop trash when ToTrash (Recycle Bin of WinSCP)
  int AddDelete(const QStringList & Paths, bool Remote, bool ToTrash = false);

  QList<QueueItemInfo> Items() const;   // in queue order
  bool IsEmpty() const;
  int ActiveCount() const;              // not finished: pending, connecting, processing, paused, waiting for user
  // First item waiting for the user (query/prompt/error), 0 = none
  int UserActionItem() const;

  void Pause(int Id);
  void Resume(int Id);                  // resumes suspended item, "Execute Now" for a pending one
  void Delete(int Id);                  // cancels if processing
  void MoveUp(int Id);
  void MoveDown(int Id);
  void PauseAll();
  void ResumeAll();
  void DeleteAllDone();
  void DeleteAll();                     // "Cancel All"

  // Shows the pending query/prompt/error of the item through the UI and
  // passes the answer to the background connection (TQueueItemProxy::ProcessUserAction).
  // For a failed item the error is shown; after it the item is removed, or
  // retried when the connection could not be opened and the user answers OK.
  // Returns false when the item has no user action. GUI thread only.
  bool ProcessUserAction(int Id);

  // Speed limit of a running/pending item (QueueItemSpeedAction), bytes/s, 0 = unlimited
  void SetItemCPSLimit(int Id, unsigned long Limit);
  // Files of a parallel transfer and their state (TQueueFileList); empty for other items
  QList<QPair<QString, QueueFileState>> ItemFileList(int Id) const;

  void SetEnabled(bool Enabled);        // "Queue > Process Queue"; when off, pending items wait
  bool Enabled() const;
  void SetTransfersLimit(int Limit);    // default QueueTransfersLimit (2)
  int TransfersLimit() const;
  // Done items are removed after Seconds (QueueKeepDoneItemsFor); 0 = at once, -1 = never
  void SetKeepDoneItemsFor(int Seconds);
  int KeepDoneItemsFor() const;
  // Single files of at least KB kilobytes are downloaded in parallel segments; <= 0 = off
  void SetParallelTransferThreshold(int KB);
  // A new connection is added to a parallel transfer only while it is expected
  // to take at least Seconds more (ParallelDurationThreshold)
  void SetParallelDurationThreshold(int Seconds);
  // Delay before a lost background connection is reopened (SessionReopenBackground)
  void SetReopenBackgroundDelay(int Ms);
  // false: queries/prompts are passed to UI at once instead of waiting for
  // ProcessUserAction (for non-interactive users of the queue)
  void SetUserActionsDeferred(bool Deferred);
  // Password/passphrase typed in the main session (TTerminal::FRememberedPassword):
  // answers the first password (or passphrase) prompt of each background connection
  void SetRememberedPassword(const QString & Password, UserInterface::PromptKind Kind = UserInterface::pkPassword);
  // Re-reads the options from Configuration::Instance() (all of them, explicit ones too)
  void ApplyConfiguration();

signals:
  void Changed();                       // any item added/removed/state changed (not on progress ticks)
  void ItemFinished(int Id, const OperationResult & Result, bool ToRemote, const QString & TargetDirectory, bool Move);
  void UserActionPending(int Id);       // item started waiting for the user (qePendingUserAction)
  void Emptied();                       // the last unfinished item finished (qeEmpty)

private:
  struct Private;
  Private * d;
};
