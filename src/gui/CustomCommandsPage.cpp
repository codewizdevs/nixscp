// Port of the custom commands part of WinSCP source/forms/Preferences.cpp
// (CustomCommandsViewData, UpdateCustomCommandsView, AddEditCommand,
// RemoveCommandButtonClick, CustomCommandMove, CustomCommandsViewKeyDown,
// CustomCommandsViewDblClick, UpdateControls) and Preferences.dfm (CustomCommandsSheet)
#include "gui/CustomCommandsPage.h"
#include "gui/HelpSystem.h"
#include "gui/CustomCommandDialog.h"

#include "core/Configuration.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QKeyEvent>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

CustomCommandsPage::CustomCommandsPage(QWidget * Parent) :
  QWidget(Parent)
{
  QVBoxLayout * Layout = new QVBoxLayout(this);
  Layout->setContentsMargins(0, 0, 0, 0);
  QGroupBox * CustomCommandsGroup = new QGroupBox(QStringLiteral("Custom commands"), this);
  QGridLayout * Grid = new QGridLayout(CustomCommandsGroup);

  CustomCommandsView = new QTreeWidget(CustomCommandsGroup);
  CustomCommandsView->setRootIsDecorated(false);
  CustomCommandsView->setUniformRowHeights(true);
  CustomCommandsView->setAllColumnsShowFocus(true);
  CustomCommandsView->setSelectionMode(QAbstractItemView::SingleSelection);
  CustomCommandsView->setHeaderLabels({QStringLiteral("Description"), QStringLiteral("Command"), QStringLiteral("L/R")});
  CustomCommandsView->header()->setSectionsClickable(false);   // ColumnClick = False
  CustomCommandsView->header()->setStretchLastSection(false);
  CustomCommandsView->header()->setSectionResizeMode(1, QHeaderView::Stretch);
  CustomCommandsView->setColumnWidth(0, 120);
  CustomCommandsView->setColumnWidth(2, 40);
  CustomCommandsView->setMinimumHeight(200);
  Grid->addWidget(CustomCommandsView, 0, 0, 1, 5);

  AddCommandButton = new QPushButton(QStringLiteral("&Add..."), CustomCommandsGroup);
  EditCommandButton = new QPushButton(QStringLiteral("&Edit..."), CustomCommandsGroup);
  RemoveCommandButton = new QPushButton(QStringLiteral("&Remove"), CustomCommandsGroup);
  UpCommandButton = new QPushButton(QStringLiteral("&Up"), CustomCommandsGroup);
  DownCommandButton = new QPushButton(QStringLiteral("&Down"), CustomCommandsGroup);
  for (QPushButton * Button : {AddCommandButton, EditCommandButton, RemoveCommandButton})
  {
    Button->setMinimumWidth(100);
  }
  UpCommandButton->setMinimumWidth(90);
  DownCommandButton->setMinimumWidth(90);
  Grid->addWidget(AddCommandButton, 1, 0);
  Grid->addWidget(EditCommandButton, 1, 1);
  Grid->addWidget(UpCommandButton, 1, 4);
  Grid->addWidget(RemoveCommandButton, 2, 0);
  Grid->addWidget(DownCommandButton, 2, 4);
  Grid->setColumnStretch(3, 1);
  Layout->addWidget(CustomCommandsGroup);

  connect(CustomCommandsView, &QTreeWidget::currentItemChanged, this, [this]() { UpdateControls(); });
  connect(CustomCommandsView, &QTreeWidget::itemSelectionChanged, this, [this]() { UpdateControls(); });
  connect(CustomCommandsView, &QTreeWidget::itemDoubleClicked, this, [this]()
  {
    // CustomCommandsViewDblClick
    if (EditCommandButton->isEnabled())
    {
      AddEditCommand(true);
    }
  });
  connect(AddCommandButton, &QPushButton::clicked, this, [this]() { AddEditCommand(false); });
  connect(EditCommandButton, &QPushButton::clicked, this, [this]() { AddEditCommand(true); });
  connect(RemoveCommandButton, &QPushButton::clicked, this, [this]() { RemoveCommand(); });
  connect(UpCommandButton, &QPushButton::clicked, this, [this]() { CustomCommandMove(CurrentIndex(), CurrentIndex() - 1); });
  connect(DownCommandButton, &QPushButton::clicked, this, [this]() { CustomCommandMove(CurrentIndex(), CurrentIndex() + 1); });

  UpdateControls();
}

CustomCommandsPage::~CustomCommandsPage() = default;

void CustomCommandsPage::SetCommands(const CustomCommandList & List)
{
  FCustomCommandList = List;
  UpdateCustomCommandsView();
  UpdateControls();
}

void CustomCommandsPage::LoadConfiguration()
{
  SetCommands(CustomCommandList::Configured());
}

void CustomCommandsPage::SaveConfiguration()
{
  CustomCommandList::SetConfigured(FCustomCommandList);
}

int CustomCommandsPage::CurrentIndex() const
{
  QTreeWidgetItem * Item = CustomCommandsView->currentItem();
  return ((Item != nullptr) && Item->isSelected()) ? CustomCommandsView->indexOfTopLevelItem(Item) : -1;
}

void CustomCommandsPage::SetCurrentIndex(int Index)
{
  QTreeWidgetItem * Item = CustomCommandsView->topLevelItem(Index);
  CustomCommandsView->setCurrentItem(Item);
  if (Item != nullptr)
  {
    Item->setSelected(true);
    CustomCommandsView->scrollToItem(Item);
  }
}

void CustomCommandsPage::UpdateCustomCommandsView()
{
  // CustomCommandsViewData
  int Index = CurrentIndex();
  CustomCommandsView->clear();
  for (const CustomCommandType & Command : FCustomCommandList.Items())
  {
    QTreeWidgetItem * Item = new QTreeWidgetItem(CustomCommandsView);
    Item->setText(0, Command.DisplayName());
    Item->setText(1, Command.Command);
    // CUSTOM_COMMAND_LOCAL / CUSTOM_COMMAND_REMOTE
    Item->setText(2, Command.IsLocal() ? QStringLiteral("L") : QStringLiteral("R"));
    Item->setToolTip(0, Command.DisplayName());
    Item->setToolTip(1, Command.Command);
  }
  if ((Index >= 0) && (Index < FCustomCommandList.Count()))
  {
    SetCurrentIndex(Index);
  }
}

void CustomCommandsPage::UpdateControls()
{
  int Index = CurrentIndex();
  bool CommandSelected = (Index >= 0);
  EditCommandButton->setEnabled(CommandSelected);
  RemoveCommandButton->setEnabled(CommandSelected);
  UpCommandButton->setEnabled(CommandSelected && (Index > 0));
  DownCommandButton->setEnabled(CommandSelected && (Index < FCustomCommandList.Count() - 1));
}

void CustomCommandsPage::AddEditCommand(bool Edit)
{
  CustomCommandType Command;
  int Index = CurrentIndex();
  if (Edit)
  {
    if (Index < 0)
    {
      return;
    }
    Command = FCustomCommandList.At(Index);
  }

  // GetShortCuts
  QList<int> ShortCuts = FOtherShortCuts + FCustomCommandList.ShortCuts();

  if (CustomCommandDialog::Execute(this, Command, FCustomCommandList,
        (Edit ? CustomCommandDialog::ccmEdit : CustomCommandDialog::ccmAdd), 0, ShortCuts))
  {
    if (Edit)
    {
      FCustomCommandList.Change(Index, Command);
    }
    // AddCommandToList: insert before the selected command, else append
    else if (Index >= 0)
    {
      FCustomCommandList.Insert(Index, Command);
    }
    else
    {
      FCustomCommandList.Add(Command);
      Index = FCustomCommandList.Count() - 1;
    }

    UpdateCustomCommandsView();
    SetCurrentIndex(Index);
    UpdateControls();
    emit Changed();
  }
}

void CustomCommandsPage::RemoveCommand()
{
  int Index = CurrentIndex();
  if (Index < 0)
  {
    return;
  }
  FCustomCommandList.Delete(Index);
  UpdateCustomCommandsView();
  if (FCustomCommandList.Count() > 0)
  {
    SetCurrentIndex(qMin(Index, FCustomCommandList.Count() - 1));
  }
  UpdateControls();
  emit Changed();
}

void CustomCommandsPage::CustomCommandMove(int Source, int Dest)
{
  if ((Source < 0) || (Dest < 0) || (Dest >= FCustomCommandList.Count()))
  {
    return;
  }
  FCustomCommandList.Move(Source, Dest);
  UpdateCustomCommandsView();
  SetCurrentIndex(Dest);
  UpdateControls();
  emit Changed();
}

void CustomCommandsPage::keyPressEvent(QKeyEvent * Event)
{
  // CustomCommandsViewKeyDown
  if ((Event->key() == Qt::Key_Delete) && RemoveCommandButton->isEnabled())
  {
    RemoveCommand();
    return;
  }
  if ((Event->key() == Qt::Key_Insert) && AddCommandButton->isEnabled())
  {
    AddEditCommand(false);
    return;
  }
  QWidget::keyPressEvent(Event);
}

bool CustomCommandsPage::Execute(QWidget * Parent)
{
  QDialog Dialog(Parent);
  Dialog.setWindowTitle(QStringLiteral("Custom commands"));
  Dialog.resize(560, 460);
  QVBoxLayout * Layout = new QVBoxLayout(&Dialog);
  CustomCommandsPage * Page = new CustomCommandsPage(&Dialog);
  Page->LoadConfiguration();
  Layout->addWidget(Page, 1);
  QDialogButtonBox * Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Help, &Dialog);
  Buttons->button(QDialogButtonBox::Help)->setText(QStringLiteral("&Help"));
  Layout->addWidget(Buttons);
  QObject::connect(Buttons, &QDialogButtonBox::accepted, &Dialog, &QDialog::accept);
  QObject::connect(Buttons, &QDialogButtonBox::rejected, &Dialog, &QDialog::reject);
  HelpSystem::InstallFormHelp(&Dialog, QStringLiteral("ui_pref_commands"), Buttons->button(QDialogButtonBox::Help));
  bool Result = (Dialog.exec() == QDialog::Accepted);
  if (Result)
  {
    Page->SaveConfiguration();
    Configuration::Instance().Save();
  }
  return Result;
}
