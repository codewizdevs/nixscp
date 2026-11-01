// Port of WinSCP source/forms/CustomCommand.{cpp,dfm} (TCustomCommandDialog,
// DoCustomCommandDialog): "Add Custom Command" / "Edit Custom Command" /
// ad hoc "Custom Command" (Commands > Custom Commands > Enter...).
#pragma once

#include "core/CustomCommands.h"

#include <QDialog>
#include <QList>
#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;

class CustomCommandDialog : public QDialog
{
  Q_OBJECT

public:
  // TCustomCommandsMode
  enum Mode { ccmAdd, ccmEdit, ccmAdHoc };
  // Options (windows/WinInterface.h)
  static constexpr int ccoDisableRemote = 0x01;        // no session: local commands only
  static constexpr int ccoDisableRemoteFiles = 0x02;   // "Use remote files" not possible
  // Additional validation on OK; return an error message to keep the dialog open
  using ValidateCallback = std::function<QString(const CustomCommandType & Command)>;

  // DoCustomCommandDialog. List: the current commands (description
  // uniqueness, Add/Edit only). UsedShortCuts: shortcuts taken by other
  // commands/bookmarks (not offered; the command's own shortcut is always
  // offered). In ccmAdHoc mode the description and shortcut are hidden;
  // an empty Command.Name becomes "Ad Hoc" (CUSTOM_COMMAND_AD_HOC_NAME).
  static bool Execute(QWidget * Parent, CustomCommandType & Command, const CustomCommandList & List, Mode AMode,
    int Options = 0, const QList<int> & UsedShortCuts = QList<int>(), ValidateCallback OnValidate = ValidateCallback());

  // CUSTOM_COMMAND_PATTERNS_HINT5 with HINT6, PATTERNS_HINT_K and PATTERNS_HINT_BACKSLASH
  static QString PatternsHint();

  ~CustomCommandDialog() override;

  void accept() override;

private:
  CustomCommandDialog(QWidget * Parent, const CustomCommandList & List, Mode AMode, int Options,
    const QList<int> & UsedShortCuts, ValidateCallback OnValidate);

  const CustomCommandList & FCustomCommandList;
  Mode FMode;
  int FOptions;
  int FParams = 0;
  QString FOrigDescription;
  ValidateCallback FOnValidate;

  QLabel * DescriptionLabel;
  QLineEdit * DescriptionEdit;
  QComboBox * CommandEdit;
  QRadioButton * RemoteCommandButton;
  QRadioButton * LocalCommandButton;
  QCheckBox * ApplyToDirectoriesCheck;
  QCheckBox * RecursiveCheck;
  QCheckBox * ShowResultsCheck;
  QCheckBox * RemoteFilesCheck;
  QCheckBox * CopyResultsCheck;
  QLabel * ShortCutLabel;
  QComboBox * ShortCutCombo;
  QPushButton * OkButton;

  void UpdateControls();
  void SetParams(int Value);
  int GetParams() const;
  void SetShortCut(int ShortCut);
  void GetCommand(CustomCommandType & Command) const;
};
