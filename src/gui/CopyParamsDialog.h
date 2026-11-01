// Port of WinSCP source/forms/CopyParams.{cpp,dfm} (TCopyParamsFrame),
// source/forms/CopyParamCustom.{cpp,dfm} ("Transfer settings" dialog),
// source/forms/CopyParamPreset.{cpp,dfm} (Add/Edit transfer settings preset)
// and the transfer settings presets menu of source/windows/WinInterface.cpp
// (CopyParamListPopup / CopyParamListPopupClick), with the permissions popup
// of source/forms/Rights.cpp reduced to a menu of checkable actions.
#pragma once

#include "core/CopyParam.h"
#include "core/CopyParamList.h"

#include <QDialog>
#include <QWidget>

class QCheckBox;
class QPoint;
class QShowEvent;

// TCopyParamsFrame
class CopyParamsFrame : public QWidget
{
  Q_OBJECT

public:
  explicit CopyParamsFrame(QWidget * Parent = nullptr);
  ~CopyParamsFrame() override;

  void SetParams(const CopyParam & Value);
  // Invalid masks/speed/permissions keep their previous values (Validate checks them)
  CopyParam Params() const;
  // cpa* flags of settings not applicable in the context; 0 enables everything
  void SetCopyParamAttrs(int Value);
  int CopyParamAttrs() const;

  // ValidateMaskComboExit / RightsEditExit / SpeedComboExit: shows the error,
  // focuses the control and returns false
  bool Validate();
  // Loads the history of the mask and speed combo boxes
  void BeforeExecute();
  // Saves the history of the mask and speed combo boxes ("Mask", "IncludeMask", "SpeedLimit")
  void AfterExecute();

private:
  struct Private;
  Private * d;
};

class CopyParamsDialog : public QDialog
{
  Q_OBJECT

public:
  // CopyParamAttrs: cpa* flags of settings not applicable in the context,
  // e.g. CopyParam::UsableAttrs(ToRemote); 0 enables everything (Preferences)
  CopyParamsDialog(const CopyParam & Param, int CopyParamAttrs = 0, QWidget * Parent = nullptr);

  CopyParam Param() const;

  // DoCopyParamCustomDialog: shows the dialog, updates Param on OK
  static bool Execute(CopyParam & Param, int CopyParamAttrs, QWidget * Parent);

  void accept() override;

private:
  CopyParamsFrame * FFrame;
};

// TCopyParamPresetDialog
class CopyParamPresetDialog : public QDialog
{
  Q_OBJECT

public:
  enum Mode { cpmAdd, cpmAddCurrent, cpmEdit, cpmDuplicate };

  // DoCopyParamPresetDialog: on OK the preset is inserted to (or changed in)
  // List at Index. Add: Index < 0 appends; Index is the new preset's index on return.
  // CurrentRuleData (optional) fills the "Current" button / cpmAddCurrent.
  static bool Execute(QWidget * Parent, CopyParamList & List, int & Index, Mode AMode,
    const CopyParamRuleData * CurrentRuleData, const CopyParam & DefaultCopyParams);

  void accept() override;
  ~CopyParamPresetDialog() override;

private:
  CopyParamPresetDialog(QWidget * Parent, Mode AMode, const CopyParamRuleData * CurrentRuleData);
  struct Private;
  Private * d;
};

// Preferences > Transfer: "Transfer settings presets" group
// (TPreferencesDialog CopyParamList* controls). Edits a copy of the presets
// and the default transfer settings in place (*DefaultParam).
class CopyParamListFrame : public QWidget
{
  Q_OBJECT

public:
  explicit CopyParamListFrame(CopyParam * DefaultParam, QWidget * Parent = nullptr);
  ~CopyParamListFrame() override;

  void SetList(const CopyParamList & List);
  CopyParamList List() const;
  // Rule data of the current session for the "Current" button (optional)
  void SetCurrentRuleData(const CopyParamRuleData * Data);
  QCheckBox * AutoSelectNoticeCheck() const;
  void UpdateControls();

signals:
  void Changed();

protected:
  void showEvent(QShowEvent * Event) override;

private:
  struct Private;
  Private * d;
};

// Options of CopyParamListPopup
const int cplNone = 0x00;
const int cplCustomizeDefault = 0x02;
const int cplSaveSettings = 0x04;

// The presets menu (Copy dialog "Transfer settings" drop down, right click on
// its transfer settings group): Custom..., Set as Default, Save as Preset...,
// Default, the presets, Configure.... Param/Preset are the current settings and
// preset name. Returns 1 when Param was replaced (by a preset or edited),
// 0 otherwise (SaveSettings may have been toggled).
int CopyParamListPopup(QWidget * Parent, const QPoint & GlobalPos, CopyParam & Param, QString & Preset,
  int Options, int CopyParamAttrs, bool * SaveSettings = nullptr);
