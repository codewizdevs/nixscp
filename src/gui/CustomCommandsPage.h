// Port of the "Commands" page of WinSCP source/forms/Preferences.{cpp,dfm}
// (CustomCommandsSheet: CustomCommandsView, Add/Edit/Remove/Up/Down;
// extensions are not ported). Embeddable in the Preferences dialog, or shown
// standalone with CustomCommandsPage::Execute.
#pragma once

#include "core/CustomCommands.h"

#include <QWidget>

class QPushButton;
class QTreeWidget;

class CustomCommandsPage : public QWidget
{
  Q_OBJECT

public:
  explicit CustomCommandsPage(QWidget * Parent = nullptr);
  ~CustomCommandsPage() override;

  void SetCommands(const CustomCommandList & List);
  const CustomCommandList & Commands() const { return FCustomCommandList; }

  // Shortcuts used elsewhere (e.g. shared bookmarks), not offered for commands
  void SetOtherShortCuts(const QList<int> & ShortCuts) { FOtherShortCuts = ShortCuts; }

  // Preferences integration: load from / store to Configuration::Instance()
  // (CustomCommandList::Configured / SetConfigured); call
  // Configuration::Save() after SaveConfiguration()
  void LoadConfiguration();
  void SaveConfiguration();

  // Standalone "Custom commands" dialog for Configuration::Instance();
  // saves the configuration on OK
  static bool Execute(QWidget * Parent);

signals:
  void Changed();

protected:
  void keyPressEvent(QKeyEvent * Event) override;

private:
  CustomCommandList FCustomCommandList;
  QList<int> FOtherShortCuts;
  QTreeWidget * CustomCommandsView;
  QPushButton * AddCommandButton;
  QPushButton * EditCommandButton;
  QPushButton * RemoveCommandButton;
  QPushButton * UpCommandButton;
  QPushButton * DownCommandButton;

  int CurrentIndex() const;
  void SetCurrentIndex(int Index);
  void UpdateCustomCommandsView();
  void UpdateControls();
  void AddEditCommand(bool Edit);
  void RemoveCommand();
  void CustomCommandMove(int Source, int Dest);
};
