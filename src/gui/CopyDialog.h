// Port of WinSCP source/forms/Copy.{cpp,dfm} (TCopyDialog, DoCopyDialog,
// CopyDialogValidateLocalDirectory, CopyDialogValidateFileMask) and the
// dialog part of TCustomScpExplorerForm::CopyParamDialog
// (ConfirmTransferring / DDTransferConfirmation, "Do not show this dialog box
// again", "Set as Default"). "Transfer settings..." opens gui/CopyParamsDialog,
// its drop down the transfer settings presets menu.
// Removed: "Duplicate" switching by typing a remote path (cooRemoteTransfer:
// on Linux local paths look like remote ones), Generate Code, Explore.
#pragma once

#include "core/CopyParam.h"

#include <QDialog>
#include <QStringList>

class CopyDialog : public QDialog
{
  Q_OBJECT

public:
  // Options (TCopyDialog co* flags)
  enum
  {
    coTemp = 0x01,            // download to a temporary directory: no target, no queue
    coDisableQueue = 0x04,    // the protocol cannot transfer in background
    coDoNotShowAgain = 0x20,  // honour ConfirmTransferring (exec() returns Accepted without
                              // showing the dialog when it is off) and offer
                              // "Do not show this dialog box again"; on by default
    coShortCutHint = 0x80,    // Commander F5 hint panel (once, CopyShortCutHintShown)
  };
  // Output options (cooDoNotShowAgain / cooSaveSettings)
  enum
  {
    cooDoNotShowAgain = 0x01,
    cooSaveSettings = 0x08,
  };

  // ToRemote: Upload, else Download. FileNames: display names of files being transferred.
  CopyDialog(bool ToRemote, bool Move, const QStringList & FileNames, const QString & TargetDirectory,
    const CopyParam & Param, QWidget * Parent = nullptr);
  ~CopyDialog() override;

  void SetOptions(int Options);
  int Options() const;
  // cpa* flags of transfer settings not applicable (e.g. CopyParam::UsableAttrs(ToRemote, Terminal));
  // default CopyParam::UsableAttrs(ToRemote)
  void SetCopyParamAttrs(int Attrs);
  // Drag & drop transfer: DDTransferConfirmation applies too, and "Do not show
  // again" turns it off (TCustomScpExplorerForm::HandleDoNotShowCopyDialogAgain)
  void SetDragDrop(bool DragDrop);
  int OutputOptions() const;

  // Shows the dialog unless the confirmation is turned off (see coDoNotShowAgain).
  // On OK: saves the target history, "Set as Default" stores the transfer
  // settings as default, "Do not show again" turns the confirmation off
  // (and the configuration is saved).
  int exec() override;
  void accept() override;
  // True when exec() accepted without showing the dialog
  bool Skipped() const;

  QString TargetDirectory() const;   // directory part of the target (without trailing slash, "/" for root)
  QString FileMask() const;          // name part of the target: "*.*", "*.bak" or a new file name
  CopyParam Param() const;          // Param.Queue reflects the queue check box, Param.FileMask the name part
  bool Queue() const;                // "Transfer in background (add to transfer queue)"

  // Whether the Copy dialog is shown at all for a (drag & drop) transfer
  // (Confirm && ConfirmTransferring; drag & drop: also DDTransferConfirmation)
  static bool ConfirmationEnabled(bool DragDrop = false);

private:
  struct Private;
  Private * d;
};
