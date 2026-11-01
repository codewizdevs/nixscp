// Port of WinSCP source/forms/SelectMask.{cpp,dfm} (Select / Deselect / Filter
// dialog with mask history and "Apply to directories") and
// source/forms/EditMask.{cpp,dfm} (mask editor: include/exclude file and
// directory masks), plus the THistoryComboBox persistence
// (TCustomWinConfiguration::History, Configuration\History\<key>) and
// TWinConfiguration::SelectMask / SelectDirectories they use.
#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

class QComboBox;

class EditMaskDialog : public QDialog
{
  Q_OBJECT

public:
  enum Mode { Select, Unselect, Filter, FileColor };

  // TSelectMaskDialog. Select/Unselect: when Mask is empty, the remembered
  // WinSCP "SelectMask"/"SelectDirectories" are offered, and the result is
  // remembered again. Filter: Mask is the current filter (empty = none), the
  // "Clear" button empties it; ApplyToDirectories is not used.
  static bool Execute(QWidget * Parent, Mode AMode, QString & Mask, bool & ApplyToDirectories);

  // DoFileColorDialog (TSelectMaskDialog in smFileColor mode, "File color"):
  // mask and its color (Preferences > Panels > File colors); Color is a BGR TColor
  static bool ExecuteFileColor(QWidget * Parent, QString & Mask, int & Color);

  // TEditMaskDialog ("Edit file mask"); Mask is replaced by the composed mask
  static bool EditMask(QWidget * Parent, QString & Mask);

  // Validates a mask (TFileMasks syntax). On error shows the WinSCP error
  // message box, selects the erroneous part in Combo (if given) and returns false.
  static bool ValidateMask(QWidget * Parent, const QString & Mask, QComboBox * Combo = nullptr);

  // TFileMasks::ComposeMaskStr (throws FileMasksException)
  static QString ComposeMaskStr(const QStringList & IncludeFileMasks, const QStringList & ExcludeFileMasks,
    const QStringList & IncludeDirectoryMasks, const QStringList & ExcludeDirectoryMasks);

  // History lists of THistoryComboBox ("Mask", "RemoteDirectory", ...), max 30 items
  static QStringList History(const QString & Key);
  static void SaveToHistory(const QString & Key, const QString & Value);
  // Fills a combo box with the history and connects nothing (call SaveToHistory on accept)
  static void LoadHistoryCombo(QComboBox * Combo, const QString & Key);

  // TWinConfiguration::SelectMask / SelectDirectories (Configuration\Interface)
  static QString SelectMask();
  static void SetSelectMask(const QString & Mask);
  static bool SelectDirectories();
  static void SetSelectDirectories(bool Value);

  // "mask hints" tooltip texts (MASK_HINT2, FILE_MASK_EX_HINT, ...)
  static QString MaskHints(bool PathMask, bool DirectoryMask);

  ~EditMaskDialog() override;

private:
  EditMaskDialog(Mode AMode, QWidget * Parent);
  struct Private;
  Private * d;
};
