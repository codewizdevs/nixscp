// Port of WinSCP source/forms/LocationProfiles.{cpp,dfm} and
// TScpCommanderForm::DoOpenDirectoryDialog / OpenBookmark (forms/ScpCommander.cpp)
// (drag & drop of bookmarks within the trees is not ported; Up/Down/Move To cover moving)
#include "gui/LocationProfilesDialog.h"
#include "gui/HelpSystem.h"
#include "gui/OpenDirectoryDialog.h"

#include "core/Bookmarks.h"
#include "core/Configuration.h"
#include "core/CustomCommands.h"
#include "core/FileSystem.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QStyle>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

// VCL TShortCut modifier flags
static const int scShift = 0x2000;
static const int scCtrl = 0x4000;
static const int scAlt = 0x8000;

enum ItemRoles { IsBookmarkRole = Qt::UserRole, NodeRole, NameRole };

static bool IsNumber(const QString & Str)
{
  bool Ok = false;
  Str.toLongLong(&Ok);
  return Ok;
}

//---------------------------------------------------------------------------
// Shortcut conversion
//---------------------------------------------------------------------------

static int VirtualKeyToQtKey(int VK)
{
  if (((VK >= '0') && (VK <= '9')) || ((VK >= 'A') && (VK <= 'Z')))
  {
    return VK;   // Qt::Key_0.. and Qt::Key_A.. have the same values
  }
  if ((VK >= 0x70) && (VK <= 0x87))   // VK_F1..VK_F24
  {
    return Qt::Key_F1 + (VK - 0x70);
  }
  if ((VK >= 0x60) && (VK <= 0x69))   // VK_NUMPAD0..9
  {
    return Qt::Key_0 + (VK - 0x60);
  }
  switch (VK)
  {
    case 0x08: return Qt::Key_Backspace;
    case 0x09: return Qt::Key_Tab;
    case 0x0D: return Qt::Key_Return;
    case 0x1B: return Qt::Key_Escape;
    case 0x20: return Qt::Key_Space;
    case 0x21: return Qt::Key_PageUp;
    case 0x22: return Qt::Key_PageDown;
    case 0x23: return Qt::Key_End;
    case 0x24: return Qt::Key_Home;
    case 0x25: return Qt::Key_Left;
    case 0x26: return Qt::Key_Up;
    case 0x27: return Qt::Key_Right;
    case 0x28: return Qt::Key_Down;
    case 0x2D: return Qt::Key_Insert;
    case 0x2E: return Qt::Key_Delete;
  }
  return 0;
}

static int QtKeyToVirtualKey(int Key)
{
  if (((Key >= Qt::Key_0) && (Key <= Qt::Key_9)) || ((Key >= Qt::Key_A) && (Key <= Qt::Key_Z)))
  {
    return Key;
  }
  if ((Key >= Qt::Key_F1) && (Key <= Qt::Key_F24))
  {
    return 0x70 + (Key - Qt::Key_F1);
  }
  for (int VK = 1; VK < 0x100; VK++)
  {
    if (VirtualKeyToQtKey(VK) == Key)
    {
      return VK;
    }
  }
  return 0;
}

QKeySequence LocationProfilesDialog::ShortCutToKeySequence(int ShortCut)
{
  int Key = VirtualKeyToQtKey(ShortCut & 0xFF);
  if ((ShortCut == 0) || (Key == 0))
  {
    return QKeySequence();
  }
  Qt::KeyboardModifiers Modifiers;
  if (ShortCut & scShift) Modifiers |= Qt::ShiftModifier;
  if (ShortCut & scCtrl) Modifiers |= Qt::ControlModifier;
  if (ShortCut & scAlt) Modifiers |= Qt::AltModifier;
  return QKeySequence(QKeyCombination(Modifiers, static_cast<Qt::Key>(Key)));
}

int LocationProfilesDialog::KeySequenceToShortCut(const QKeySequence & Sequence)
{
  if (Sequence.isEmpty())
  {
    return 0;
  }
  QKeyCombination Combination = Sequence[0];
  int VK = QtKeyToVirtualKey(Combination.key());
  if (VK == 0)
  {
    return 0;
  }
  Qt::KeyboardModifiers Modifiers = Combination.keyboardModifiers();
  return VK | ((Modifiers & Qt::ShiftModifier) ? scShift : 0) | ((Modifiers & Qt::ControlModifier) ? scCtrl : 0) |
    ((Modifiers & Qt::AltModifier) ? scAlt : 0);
}

QString LocationProfilesDialog::ShortCutToText(int ShortCut)
{
  if (ShortCut == 0)
  {
    return QString();
  }
  QString Result;
  if (ShortCut & scShift) Result += QStringLiteral("Shift+");
  if (ShortCut & scCtrl) Result += QStringLiteral("Ctrl+");
  if (ShortCut & scAlt) Result += QStringLiteral("Alt+");
  int Key = VirtualKeyToQtKey(ShortCut & 0xFF);
  Result += (Key != 0) ? QKeySequence(Key).toString(QKeySequence::NativeText) : QStringLiteral("?");
  return Result;
}

//---------------------------------------------------------------------------
// Small dialogs (TCustomDialog descendants)
//---------------------------------------------------------------------------

namespace
{

class ComboDialog : public QDialog
{
public:
  // TCustomDialog(HelpKeyword): HELP_LOCATION_PROFILE_ADD / _MOVE, the shortcut
  // dialog gets the keyword of its owner (DoShortCutDialog(..., HelpKeyword))
  ComboDialog(QWidget * Parent, const QString & Caption, const QString & Prompt, const QStringList & Items, bool Editable,
    const QString & HelpKeyword = QStringLiteral("ui_locationprofile")) :
    QDialog(Parent)
  {
    setWindowTitle(Caption);
    Layout = new QVBoxLayout(this);
    Combo = new QComboBox(this);
    Combo->setEditable(Editable);
    Combo->setInsertPolicy(QComboBox::NoInsert);
    Combo->addItems(Items);
    Combo->setMinimumContentsLength(30);
    Combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    QLabel * Label = new QLabel(Prompt, this);
    Label->setBuddy(Combo);
    Layout->addWidget(Label);
    Layout->addWidget(Combo);
    Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    HelpSystem::AddHelpButton(Buttons, this, HelpKeyword);
    connect(Buttons, &QDialogButtonBox::accepted, this, [this]()
    {
      if (!Validate || Validate(Combo->currentText()))
      {
        accept();
      }
    });
    connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout()->setSizeConstraint(QLayout::SetFixedSize);
  }

  void Finish()
  {
    Layout->addSpacing(6);
    Layout->addWidget(Buttons);
  }

  QVBoxLayout * Layout;
  QComboBox * Combo;
  QDialogButtonBox * Buttons;
  std::function<bool(const QString &)> Validate;
};

} // namespace

bool LocationProfilesDialog::SelectShortCut(QWidget * Parent, int & ShortCut, const QList<int> & Used)
{
  // forms/Custom.cpp TShortCutDialog + InitializeShortCutCombo
  QString HelpKeyword = (Parent != nullptr) ? Parent->window()->property("HelpKeyword").toString() : QString();
  ComboDialog Dialog(Parent, QStringLiteral("Select keyboard shortcut"), QStringLiteral("&Keyboard shortcut:"), QStringList(), false,
    HelpKeyword.isEmpty() ? QStringLiteral("ui_locationprofile") : HelpKeyword);
  Dialog.Finish();
  Dialog.Combo->addItem(QStringLiteral("None"), 0);
  QList<int> Available;
  for (int Key = '0'; Key <= '9'; Key++)
  {
    Available.append(scCtrl | Key);
  }
  for (int Key = 'A'; Key <= 'Z'; Key++)
  {
    Available.append(scShift | scCtrl | scAlt | Key);
  }
  for (int Item : Available)
  {
    if (!Used.contains(Item) || (Item == ShortCut))
    {
      Dialog.Combo->addItem(ShortCutToText(Item), Item);
    }
  }
  int Current = Dialog.Combo->findData(ShortCut);
  Dialog.Combo->setCurrentIndex((Current >= 0) ? Current : 0);
  if (Dialog.exec() != QDialog::Accepted)
  {
    return false;
  }
  ShortCut = Dialog.Combo->currentData().toInt();
  return true;
}

//---------------------------------------------------------------------------
// TLocationProfilesDialog
//---------------------------------------------------------------------------

struct LocationProfilesDialog::Private : public QObject
{
  struct Page
  {
    bool Shared = false;
    QWidget * Sheet = nullptr;
    QTreeWidget * View = nullptr;
    BookmarkList List;
    bool Modified = false;
    QPushButton * AddButton = nullptr;
    QPushButton * RemoveButton = nullptr;
    QPushButton * RenameButton = nullptr;
    QPushButton * MoveToButton = nullptr;
    QPushButton * ShortCutButton = nullptr;
    QPushButton * UpButton = nullptr;
    QPushButton * DownButton = nullptr;
  };

  LocationProfilesDialog * Q = nullptr;
  QString SessionKey;
  bool RemoteSide = false;
  bool AddMode = false;
  bool Changing = false;
  bool Switched = false;
  bool Rebuilding = false;
  QComboBox * LocalDirectoryEdit = nullptr;
  QComboBox * RemoteDirectoryEdit = nullptr;
  QTabWidget * PageControl = nullptr;
  QPushButton * OkButton = nullptr;
  Page Pages[2];   // 0 = site, 1 = shared

  QString LocalDirectory() const
  {
    // ExcludeTrailingBackslash
    QString Result = LocalDirectoryEdit->currentText().trimmed();
    while ((Result.size() > 1) && Result.endsWith(QLatin1Char('/')))
    {
      Result.chop(1);
    }
    return Result;
  }

  QString RemoteDirectory() const
  {
    QString Result = RemoteDirectoryEdit->currentText().trimmed();
    while ((Result.size() > 1) && Result.endsWith(QLatin1Char('/')))
    {
      Result.chop(1);
    }
    return Result;
  }

  Page & ActivePage() { return Pages[(PageControl->currentIndex() == 1) ? 1 : 0]; }

  static bool IsBookmarkItem(const QTreeWidgetItem * Item)
  {
    return (Item != nullptr) && Item->data(0, IsBookmarkRole).toBool();
  }

  static int BookmarkIndex(const Page & P, const QTreeWidgetItem * Item)
  {
    if (!IsBookmarkItem(Item))
    {
      return -1;
    }
    return P.List.IndexOf(Item->data(0, NodeRole).toString(), Item->data(0, NameRole).toString());
  }

  static QString BookmarkText(const Bookmark & B)
  {
    QString Result = B.Name;
    if (!Result.isEmpty() && (B.ShortCut != 0))
    {
      Result = QStringLiteral("%1 (%2)").arg(Result, ShortCutToText(B.ShortCut));
    }
    return Result;
  }

  // TLocationProfilesDialog::LoadBookmarks; selects the given bookmark or folder afterwards
  void Rebuild(Page & P, const QString & SelectNode = QString(), const QString & SelectName = QString(),
    const QString & SelectFolder = QString())
  {
    Rebuilding = true;
    QTreeWidget * View = P.View;
    View->clear();
    QIcon FolderIcon = Q->style()->standardIcon(QStyle::SP_DirIcon);
    QIcon BookmarkIcon = Q->style()->standardIcon(QStyle::SP_DirLinkIcon);
    QMap<QString, QTreeWidgetItem *> Folders;
    for (const QString & Folder : P.List.Folders())
    {
      QTreeWidgetItem * Item = new QTreeWidgetItem(View);
      Item->setText(0, Folder);
      Item->setIcon(0, FolderIcon);
      Item->setData(0, IsBookmarkRole, false);
      Item->setData(0, NodeRole, Folder);
      Item->setFlags(Item->flags() | Qt::ItemIsEditable);
      Folders.insert(Folder.toLower(), Item);
    }
    QTreeWidgetItem * Select = nullptr;
    for (const Bookmark & B : P.List.Items())
    {
      QTreeWidgetItem * Parent = B.Node.isEmpty() ? nullptr : Folders.value(B.Node.toLower());
      QTreeWidgetItem * Item = (Parent != nullptr) ? new QTreeWidgetItem(Parent) : new QTreeWidgetItem(View);
      Item->setText(0, BookmarkText(B));
      Item->setIcon(0, BookmarkIcon);
      Item->setData(0, IsBookmarkRole, true);
      Item->setData(0, NodeRole, B.Node);
      Item->setData(0, NameRole, B.Name);
      Item->setToolTip(0, QStringLiteral("%1\n%2").arg(B.Local, B.Remote).trimmed());
      Item->setFlags(Item->flags() | Qt::ItemIsEditable);
      if (!SelectName.isEmpty() && (B.Node.compare(SelectNode, Qt::CaseInsensitive) == 0) &&
          (B.Name.compare(SelectName, Qt::CaseInsensitive) == 0))
      {
        Select = Item;
      }
    }
    for (auto It = Folders.cbegin(); It != Folders.cend(); ++It)
    {
      It.value()->setExpanded(P.List.NodeOpened(It.value()->text(0)));
      if (!SelectFolder.isEmpty() && (It.value()->text(0).compare(SelectFolder, Qt::CaseInsensitive) == 0))
      {
        Select = It.value();
      }
    }
    if (Select != nullptr)
    {
      if (Select->parent() != nullptr)
      {
        Select->parent()->setExpanded(true);
      }
      View->setCurrentItem(Select);
      View->scrollToItem(Select);
    }
    else
    {
      View->setCurrentItem(nullptr);
      View->clearSelection();
    }
    Rebuilding = false;
    UpdateControls();
  }

  void Modify(Page & P)
  {
    P.Modified = true;
  }

  bool ProfileMatch(const Page & P, const QTreeWidgetItem * Item) const
  {
    int Index = BookmarkIndex(P, Item);
    if (Index < 0)
    {
      return false;
    }
    const Bookmark & B = P.List.At(Index);
    return (B.Local == LocalDirectory()) && (B.Remote == RemoteDirectory());
  }

  // TLocationProfilesDialog::FindProfile
  void FindProfile(Page & P)
  {
    QTreeWidgetItem * Selected = P.View->currentItem();
    if ((Selected != nullptr) && Selected->isSelected() && ProfileMatch(P, Selected))
    {
      return;
    }
    QTreeWidgetItem * Match = nullptr;
    for (QTreeWidgetItemIterator It(P.View); *It != nullptr; ++It)
    {
      if (ProfileMatch(P, *It))
      {
        Match = *It;
        break;
      }
    }
    bool PrevChanging = Changing;
    Changing = true;
    if (Match != nullptr)
    {
      P.View->setCurrentItem(Match);
      P.View->scrollToItem(Match);
    }
    else
    {
      P.View->setCurrentItem(nullptr);
      P.View->clearSelection();
    }
    Changing = PrevChanging;
  }

  void FindProfile()
  {
    FindProfile(Pages[0]);
    FindProfile(Pages[1]);
  }

  void UpdateProfilesControls(Page & P)
  {
    QTreeWidgetItem * Selected = P.View->selectedItems().isEmpty() ? nullptr : P.View->currentItem();
    bool IsBookmark = IsBookmarkItem(Selected);
    P.AddButton->setEnabled(!LocalDirectory().isEmpty() || !RemoteDirectory().isEmpty());
    P.RemoveButton->setEnabled(Selected != nullptr);
    P.RenameButton->setEnabled(Selected != nullptr);
    P.MoveToButton->setEnabled(IsBookmark);
    if (P.ShortCutButton != nullptr)
    {
      P.ShortCutButton->setEnabled(IsBookmark);
    }
    QTreeWidgetItem * Parent = (Selected != nullptr) ? Selected->parent() : nullptr;
    int Position = (Selected == nullptr) ? -1 :
      ((Parent != nullptr) ? Parent->indexOfChild(Selected) : P.View->indexOfTopLevelItem(Selected));
    int Count = (Parent != nullptr) ? Parent->childCount() : P.View->topLevelItemCount();
    auto Sibling = [&](int Offset) -> QTreeWidgetItem *
    {
      int I = Position + Offset;
      if ((Position < 0) || (I < 0) || (I >= Count))
      {
        return nullptr;
      }
      return (Parent != nullptr) ? Parent->child(I) : P.View->topLevelItem(I);
    };
    P.UpButton->setEnabled(IsBookmark && IsBookmarkItem(Sibling(-1)));
    P.DownButton->setEnabled(IsBookmark && IsBookmarkItem(Sibling(1)));
  }

  void UpdateControls()
  {
    if (OkButton == nullptr)
    {
      return;
    }
    OkButton->setEnabled(!LocalDirectory().isEmpty() || !RemoteDirectory().isEmpty());
    UpdateProfilesControls(Pages[0]);
    UpdateProfilesControls(Pages[1]);
  }

  void ShowError(const QString & Message)
  {
    QMessageBox::critical(Q, QStringLiteral("Error"), Message);
  }

  // TLocationProfilesDialog::AddAsBookmark
  bool AddAsBookmark(Page * P, bool Initial)
  {
    QString BookmarkName;
    if ((!RemoteSide && !LocalDirectory().isEmpty()) || RemoteDirectory().isEmpty())
    {
      BookmarkName = LocalDirectory();
    }
    else
    {
      BookmarkName = RemoteDirectory();
    }

    QTreeWidgetItem * Selected = Initial || P->View->selectedItems().isEmpty() ? nullptr : P->View->currentItem();
    int SelectedIndex = BookmarkIndex(*P, Selected);
    QString SelectedNode;
    if (Selected != nullptr)
    {
      SelectedNode = Selected->data(0, NodeRole).toString();
    }

    QStringList PeerBookmarks;
    for (const Bookmark & B : P->List.Items())
    {
      if (B.Node == SelectedNode)
      {
        PeerBookmarks.append(B.Name);
      }
    }

    // TBookmarkNameDialog
    ComboDialog Dialog(Q, QStringLiteral("Add Location Profile"), QStringLiteral("Location Profile name:"), PeerBookmarks, true);
    QCheckBox * SharedCheck = nullptr;
    if (Initial)
    {
      SharedCheck = new QCheckBox(QStringLiteral("Add to &shared bookmarks"), &Dialog);
      SharedCheck->setChecked(Bookmarks::UseSharedBookmarks(Configuration::Instance().Ini()));
      Dialog.Layout->addWidget(SharedCheck);
    }
    Dialog.Finish();
    Dialog.Validate = [this](const QString & Name)
    {
      if (Name.isEmpty() || IsNumber(Name))
      {
        // BOOKMARK_INVALID_NAME
        ShowError(QStringLiteral("'%1' is not valid Location Profile name.").arg(Name));
        return false;
      }
      return true;
    };
    Dialog.Combo->setEditText(BookmarkName);
    Dialog.Combo->lineEdit()->selectAll();
    if (Dialog.exec() != QDialog::Accepted)
    {
      UpdateControls();
      return false;
    }
    BookmarkName = Dialog.Combo->currentText();
    if (Initial)
    {
      bool Shared = SharedCheck->isChecked();
      Bookmarks::SetUseSharedBookmarks(Configuration::Instance().Ini(), Shared);
      PageControl->setCurrentIndex(Shared ? 1 : 0);
      P = &Pages[Shared ? 1 : 0];
    }

    try
    {
      int Existing = P->List.IndexOf(SelectedNode, BookmarkName);
      if (Existing >= 0)
      {
        Bookmark B = P->List.At(Existing);
        B.Local = LocalDirectory();
        B.Remote = RemoteDirectory();
        P->List.Update(Existing, B);
        BookmarkName = B.Name;
      }
      else
      {
        Bookmark B;
        B.Name = BookmarkName;
        B.Local = LocalDirectory();
        B.Remote = RemoteDirectory();
        B.Node = SelectedNode;   // bookmark's folder, or the selected folder
        if (SelectedIndex >= 0)
        {
          P->List.Insert(SelectedIndex, B);
        }
        else
        {
          P->List.Add(B);
        }
      }
    }
    catch (const ExtException & E)
    {
      ShowError(E.Message());
      return false;
    }
    Modify(*P);
    Rebuild(*P, SelectedNode, BookmarkName);
    return true;
  }

  // TLocationProfilesDialog::RemoveBookmark
  void RemoveBookmark(Page & P)
  {
    QTreeWidgetItem * Item = P.View->currentItem();
    if ((Item == nullptr) || !Item->isSelected())
    {
      return;
    }
    if (IsBookmarkItem(Item))
    {
      int Index = BookmarkIndex(P, Item);
      if (Index >= 0)
      {
        P.List.Delete(Index);
      }
    }
    else
    {
      // DELETE_BOOKMARK_FOLDER
      if (QMessageBox::question(Q, QStringLiteral("Confirm"), QStringLiteral("Delete selected Location Profile folder?"),
            QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
      {
        return;
      }
      QString Folder = Item->text(0);
      for (int Index = P.List.Count() - 1; Index >= 0; Index--)
      {
        if (P.List.At(Index).Node.compare(Folder, Qt::CaseInsensitive) == 0)
        {
          P.List.Delete(Index);
        }
      }
      P.List.SetNodeOpened(Folder, false);
    }
    Modify(P);
    Rebuild(P);
  }

  void RenameBookmark(Page & P)
  {
    QTreeWidgetItem * Item = P.View->currentItem();
    if (Item != nullptr)
    {
      P.View->setFocus();
      // show the bare name for editing (the text may include the shortcut)
      Rebuilding = true;
      if (IsBookmarkItem(Item))
      {
        Item->setText(0, Item->data(0, NameRole).toString());
      }
      Rebuilding = false;
      P.View->editItem(Item, 0);
    }
  }

  // TLocationProfilesDialog::ProfilesViewEdited
  void ItemEdited(Page & P, QTreeWidgetItem * Item)
  {
    if (Rebuilding)
    {
      return;
    }
    QString S = Item->text(0);
    QString SelectNode;
    QString SelectName;
    QString SelectFolder;
    try
    {
      if (IsBookmarkItem(Item))
      {
        int Index = BookmarkIndex(P, Item);
        SelectNode = Item->data(0, NodeRole).toString();
        SelectName = Item->data(0, NameRole).toString();
        if ((Index >= 0) && (S != SelectName))
        {
          if (S.isEmpty() || IsNumber(S))
          {
            throw ExtException(QStringLiteral("'%1' is not valid Location Profile name.").arg(S));
          }
          Bookmark B = P.List.At(Index);
          B.Name = S;
          P.List.Update(Index, B);
          SelectName = S;
          Modify(P);
        }
      }
      else
      {
        QString OldFolder = Item->data(0, NodeRole).toString();
        SelectFolder = OldFolder;
        if (S != OldFolder)
        {
          if (S.isEmpty() || S.contains(QLatin1Char('\\')))
          {
            // BOOKMARK_FOLDER_INVALID_NAME
            throw ExtException(QStringLiteral("'%1' is not valid Location Profile folder name.").arg(S));
          }
          if (P.List.Folders().contains(S, Qt::CaseInsensitive) && (S.compare(OldFolder, Qt::CaseInsensitive) != 0))
          {
            // DUPLICATE_BOOKMARK_FOLDER
            throw ExtException(QStringLiteral("Location Profile folder with name '%1' already exists.").arg(S));
          }
          BookmarkList NewList = P.List;
          for (int Index = 0; Index < NewList.Count(); Index++)
          {
            if (NewList.At(Index).Node.compare(OldFolder, Qt::CaseInsensitive) == 0)
            {
              Bookmark B = NewList.At(Index);
              B.Node = S;
              NewList.Update(Index, B);
            }
          }
          bool Opened = NewList.NodeOpened(OldFolder);
          NewList.SetNodeOpened(OldFolder, false);
          NewList.SetNodeOpened(S, Opened);
          P.List = NewList;
          SelectFolder = S;
          Modify(P);
        }
      }
    }
    catch (const ExtException & E)
    {
      // deferred, the editor is still being closed
      QString Message = E.Message();
      QTimer::singleShot(0, this, [this, Message]() { ShowError(Message); });
    }
    // deferred: the item must not be deleted while the view handles the edit
    QTimer::singleShot(0, this, [this, &P, SelectNode, SelectName, SelectFolder]()
    {
      Rebuild(P, SelectNode, SelectName, SelectFolder);
    });
  }

  // TLocationProfilesDialog::BookmarkButtonClick (Up/Down)
  void MoveBookmark(Page & P, int Direction)
  {
    QTreeWidgetItem * Item = P.View->currentItem();
    int Index = BookmarkIndex(P, Item);
    if (Index < 0)
    {
      return;
    }
    QTreeWidgetItem * Parent = Item->parent();
    int Position = (Parent != nullptr) ? Parent->indexOfChild(Item) : P.View->indexOfTopLevelItem(Item);
    int Target = Position + Direction;
    int Count = (Parent != nullptr) ? Parent->childCount() : P.View->topLevelItemCount();
    if ((Target < 0) || (Target >= Count))
    {
      return;
    }
    QTreeWidgetItem * TargetItem = (Parent != nullptr) ? Parent->child(Target) : P.View->topLevelItem(Target);
    int TargetIndex = BookmarkIndex(P, TargetItem);
    if (TargetIndex < 0)
    {
      return;
    }
    Bookmark B = P.List.At(Index);
    P.List.Move(Index, TargetIndex);
    Modify(P);
    Rebuild(P, B.Node, B.Name);
  }

  // TLocationProfilesDialog::BookmarkMoveToButtonClick
  void MoveTo(Page & P)
  {
    QTreeWidgetItem * Item = P.View->currentItem();
    int Index = BookmarkIndex(P, Item);
    if (Index < 0)
    {
      return;
    }
    Bookmark B = P.List.At(Index);
    ComboDialog Dialog(Q, QStringLiteral("Move Location Profile"), QStringLiteral("New folder name:"), P.List.Folders(), true);
    Dialog.Finish();
    Dialog.Validate = [this](const QString & Name)
    {
      if (Name.contains(QLatin1Char('\\')))
      {
        ShowError(QStringLiteral("'%1' is not valid Location Profile folder name.").arg(Name));
        return false;
      }
      return true;
    };
    Dialog.Combo->setEditText(B.Node);
    if ((Dialog.exec() != QDialog::Accepted) || (Dialog.Combo->currentText() == B.Node))
    {
      return;
    }
    QString NodeName = Dialog.Combo->currentText();
    // use the existing folder's spelling
    for (const QString & Folder : P.List.Folders())
    {
      if (Folder.compare(NodeName, Qt::CaseInsensitive) == 0)
      {
        NodeName = Folder;
      }
    }
    try
    {
      Bookmark Moved = B;
      Moved.Node = NodeName;
      P.List.Update(Index, Moved);
      // BookmarkMove to a folder: moved to the end
      P.List.Move(Index, P.List.Count() - 1);
    }
    catch (const ExtException & E)
    {
      ShowError(E.Message());
      return;
    }
    if (!NodeName.isEmpty())
    {
      P.List.SetNodeOpened(NodeName, true);
    }
    Modify(P);
    Rebuild(P, NodeName, B.Name);
  }

  // TLocationProfilesDialog::ShortCutBookmarkButtonClick + TShortCutDialog
  void AssignShortCut(Page & P)
  {
    QTreeWidgetItem * Item = P.View->currentItem();
    int Index = BookmarkIndex(P, Item);
    if (Index < 0)
    {
      return;
    }
    Bookmark B = P.List.At(Index);
    // WinConfiguration->CustomCommandShortCuts + BookmarkList->ShortCuts
    QList<int> Used = CustomCommandList::Configured().ShortCuts() + P.List.ShortCuts();
    Used.removeAll(B.ShortCut);
    int ShortCut = B.ShortCut;
    if (!SelectShortCut(Q, ShortCut, Used))
    {
      return;
    }
    B.ShortCut = ShortCut;
    P.List.Update(Index, B);
    Modify(P);
    Rebuild(P, B.Node, B.Name);
  }

  void SelectionChanged(Page & P)
  {
    if (Rebuilding)
    {
      return;
    }
    QTreeWidgetItem * Item = P.View->currentItem();
    int Index = ((Item != nullptr) && Item->isSelected()) ? BookmarkIndex(P, Item) : -1;
    if ((Index >= 0) && !Changing)
    {
      // ProfilesViewChange
      const Bookmark & B = P.List.At(Index);
      Changing = true;
      LocalDirectoryEdit->setEditText(B.Local);
      RemoteDirectoryEdit->setEditText(B.Remote);
      Changing = false;
      // try to locate the same profile in the other set
      FindProfile(Pages[(&P == &Pages[0]) ? 1 : 0]);
    }
    UpdateControls();
  }

  void DirectoryEditChange()
  {
    if (!Changing)
    {
      FindProfile();
      UpdateControls();
    }
  }

  void CreatePage(int Index, const QString & Caption)
  {
    Page & P = Pages[Index];
    P.Shared = (Index == 1);
    P.Sheet = new QWidget(PageControl);
    QHBoxLayout * Layout = new QHBoxLayout(P.Sheet);
    P.View = new QTreeWidget(P.Sheet);
    P.View->setHeaderHidden(true);
    P.View->setColumnCount(1);
    P.View->setSelectionMode(QAbstractItemView::SingleSelection);
    P.View->setEditTriggers(QAbstractItemView::EditKeyPressed);
    P.View->installEventFilter(this);
    Layout->addWidget(P.View, 1);
    QVBoxLayout * Buttons = new QVBoxLayout();
    P.AddButton = new QPushButton(QStringLiteral("&Add..."), P.Sheet);
    P.RemoveButton = new QPushButton(QStringLiteral("Remo&ve"), P.Sheet);
    P.RenameButton = new QPushButton(QStringLiteral("Re&name"), P.Sheet);
    P.MoveToButton = new QPushButton(QStringLiteral("&Move To..."), P.Sheet);
    if (P.Shared)
    {
      P.ShortCutButton = new QPushButton(QStringLiteral("&Shortcut..."), P.Sheet);
    }
    P.UpButton = new QPushButton(QStringLiteral("&Up"), P.Sheet);
    P.DownButton = new QPushButton(QStringLiteral("&Down"), P.Sheet);
    Buttons->addWidget(P.AddButton);
    Buttons->addWidget(P.RemoveButton);
    Buttons->addWidget(P.RenameButton);
    Buttons->addWidget(P.MoveToButton);
    if (P.ShortCutButton != nullptr)
    {
      Buttons->addWidget(P.ShortCutButton);
    }
    Buttons->addStretch(1);
    Buttons->addWidget(P.UpButton);
    Buttons->addWidget(P.DownButton);
    for (QPushButton * Button : P.Sheet->findChildren<QPushButton *>())
    {
      Button->setAutoDefault(false);
      Button->setMinimumWidth(92);
    }
    Layout->addLayout(Buttons);
    PageControl->addTab(P.Sheet, Caption);

    Page * PP = &P;
    connect(P.AddButton, &QPushButton::clicked, this, [this, PP]() { AddAsBookmark(PP, false); });
    connect(P.RemoveButton, &QPushButton::clicked, this, [this, PP]() { RemoveBookmark(*PP); });
    connect(P.RenameButton, &QPushButton::clicked, this, [this, PP]() { RenameBookmark(*PP); });
    connect(P.MoveToButton, &QPushButton::clicked, this, [this, PP]() { MoveTo(*PP); });
    if (P.ShortCutButton != nullptr)
    {
      connect(P.ShortCutButton, &QPushButton::clicked, this, [this, PP]() { AssignShortCut(*PP); });
    }
    connect(P.UpButton, &QPushButton::clicked, this, [this, PP]() { MoveBookmark(*PP, -1); });
    connect(P.DownButton, &QPushButton::clicked, this, [this, PP]() { MoveBookmark(*PP, 1); });
    connect(P.View, &QTreeWidget::itemSelectionChanged, this, [this, PP]() { SelectionChanged(*PP); });
    connect(P.View, &QTreeWidget::currentItemChanged, this, [this, PP]() { SelectionChanged(*PP); });
    connect(P.View, &QTreeWidget::itemChanged, this, [this, PP](QTreeWidgetItem * Item) { ItemEdited(*PP, Item); });
    connect(P.View, &QTreeWidget::itemExpanded, this, [this, PP](QTreeWidgetItem * Item)
    {
      if (!Rebuilding && !IsBookmarkItem(Item))
      {
        PP->List.SetNodeOpened(Item->text(0), true);
        Modify(*PP);
      }
    });
    connect(P.View, &QTreeWidget::itemCollapsed, this, [this, PP](QTreeWidgetItem * Item)
    {
      if (!Rebuilding && !IsBookmarkItem(Item))
      {
        PP->List.SetNodeOpened(Item->text(0), false);
        Modify(*PP);
      }
    });
    connect(P.View, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem * Item)
    {
      // ProfilesViewDblClick
      if (OkButton->isEnabled() && IsBookmarkItem(Item) && Item->isSelected())
      {
        Q->accept();
      }
    });
  }

  bool eventFilter(QObject * Watched, QEvent * Event) override
  {
    if (Event->type() == QEvent::KeyPress)
    {
      for (Page & P : Pages)
      {
        if (Watched == P.View)
        {
          QKeyEvent * Key = static_cast<QKeyEvent *>(Event);
          // ProfilesViewKeyDown
          if ((Key->key() == Qt::Key_Delete) && (P.View->currentItem() != nullptr))
          {
            RemoveBookmark(P);
            return true;
          }
          if ((Key->key() == Qt::Key_F2) && (P.View->currentItem() != nullptr))
          {
            RenameBookmark(P);
            return true;
          }
        }
      }
    }
    return QObject::eventFilter(Watched, Event);
  }

  void SaveBookmarks()
  {
    IniFile & Ini = Configuration::Instance().Ini();
    Bookmarks All;
    All.Load(Ini);
    bool Changed = false;
    if (Pages[0].Modified)
    {
      All.SetList(SessionKey, Pages[0].List);
      Changed = true;
    }
    if (Pages[1].Modified)
    {
      All.SetSharedList(Pages[1].List);
      Changed = true;
    }
    if (Changed)
    {
      All.Save(Ini);
    }
    bool UseShared = (PageControl->currentIndex() == 1);
    if (Bookmarks::UseSharedBookmarks(Ini) != UseShared)
    {
      Bookmarks::SetUseSharedBookmarks(Ini, UseShared);
      Changed = true;
    }
    if (Changed || Switched)
    {
      Configuration::Instance().Save();
    }
    if (Changed)
    {
      RefreshBookmarkShortCuts();
    }
  }
};

LocationProfilesDialog::LocationProfilesDialog(QWidget * Parent) :
  QDialog(Parent),
  d(new Private)
{
  d->Q = this;
  setWindowTitle(QStringLiteral("Location profiles"));
  setSizeGripEnabled(true);
  QGridLayout * Layout = new QGridLayout(this);

  QLabel * Image = new QLabel(this);
  Image->setPixmap(style()->standardIcon(QStyle::SP_DirOpenIcon).pixmap(32, 32));
  Image->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
  Layout->addWidget(Image, 0, 0, 4, 1, Qt::AlignTop);

  d->LocalDirectoryEdit = new QComboBox(this);
  d->LocalDirectoryEdit->setEditable(true);
  d->LocalDirectoryEdit->setInsertPolicy(QComboBox::NoInsert);
  d->LocalDirectoryEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  d->LocalDirectoryEdit->setMinimumContentsLength(30);
  d->LocalDirectoryEdit->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  QLabel * LocalLabel = new QLabel(QStringLiteral("&Local directory:"), this);
  LocalLabel->setBuddy(d->LocalDirectoryEdit);
  QPushButton * BrowseButton = new QPushButton(QStringLiteral("B&rowse..."), this);
  BrowseButton->setAutoDefault(false);
  Layout->addWidget(LocalLabel, 0, 1, 1, 2);
  Layout->addWidget(d->LocalDirectoryEdit, 1, 1);
  Layout->addWidget(BrowseButton, 1, 2);

  d->RemoteDirectoryEdit = new QComboBox(this);
  d->RemoteDirectoryEdit->setEditable(true);
  d->RemoteDirectoryEdit->setInsertPolicy(QComboBox::NoInsert);
  d->RemoteDirectoryEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  d->RemoteDirectoryEdit->setMinimumContentsLength(30);
  d->RemoteDirectoryEdit->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  QLabel * RemoteLabel = new QLabel(QStringLiteral("&Remote directory:"), this);
  RemoteLabel->setBuddy(d->RemoteDirectoryEdit);
  Layout->addWidget(RemoteLabel, 2, 1, 1, 2);
  Layout->addWidget(d->RemoteDirectoryEdit, 3, 1, 1, 2);

  d->PageControl = new QTabWidget(this);
  d->CreatePage(0, QStringLiteral("Site Location profiles"));
  d->CreatePage(1, QStringLiteral("Shared Location profiles"));
  Layout->addWidget(d->PageControl, 4, 0, 1, 3);
  Layout->setRowStretch(4, 1);
  Layout->setColumnStretch(1, 1);

  QDialogButtonBox * Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  d->OkButton = Buttons->button(QDialogButtonBox::Ok);
  // SwitchButton: back to the Open directory dialog (ModalResult = mrCancel)
  QPushButton * SwitchButton = Buttons->addButton(QStringLiteral("&Bookmarks..."), QDialogButtonBox::ResetRole);
  SwitchButton->setObjectName(QStringLiteral("SwitchButton"));
  SwitchButton->setAutoDefault(false);
  connect(SwitchButton, &QPushButton::clicked, this, [this]()
  {
    d->Switched = true;
    Configuration::Instance().UseLocationProfiles = false;
    reject();
  });
  HelpSystem::AddHelpButton(Buttons, this, QStringLiteral("ui_locationprofile"));
  Layout->addWidget(Buttons, 5, 0, 1, 3);
  connect(Buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  connect(BrowseButton, &QPushButton::clicked, d, [this]()
  {
    QString Directory = QFileDialog::getExistingDirectory(this, QString(), d->LocalDirectory());
    if (!Directory.isEmpty())
    {
      d->LocalDirectoryEdit->setEditText(Directory);
    }
  });
  connect(d->LocalDirectoryEdit, &QComboBox::editTextChanged, d, [this]() { d->DirectoryEditChange(); });
  connect(d->RemoteDirectoryEdit, &QComboBox::editTextChanged, d, [this]() { d->DirectoryEditChange(); });
  resize(601, 448);
}

LocationProfilesDialog::~LocationProfilesDialog()
{
  delete d;
}

void LocationProfilesDialog::showEvent(QShowEvent * Event)
{
  QDialog::showEvent(Event);
  if (d->AddMode)
  {
    d->AddMode = false;
    // after the dialog is visible (FormShow)
    QTimer::singleShot(0, d, [this]()
    {
      d->AddAsBookmark(&d->ActivePage(), true);
      d->FindProfile();
      d->UpdateControls();
    });
  }
}

void LocationProfilesDialog::done(int Result)
{
  d->SaveBookmarks();
  QDialog::done(Result);
}

bool LocationProfilesDialog::Execute(QWidget * Parent, const QString & SessionKey, bool Remote,
  QString & LocalDirectory, QString & RemoteDirectory, bool AddMode,
  const QStringList & LocalDirectories, const QStringList & RemoteDirectories)
{
  // TScpCommanderForm::DoOpenDirectoryDialog
  Configuration & Config = Configuration::Instance();
  bool Result = false;
  bool UseLocationProfiles;
  bool First = true;
  do
  {
    UseLocationProfiles = Config.UseLocationProfiles;
    // Location profiles dialog is not ready to be run without terminal
    if (UseLocationProfiles && !SessionKey.isEmpty())
    {
      Result = ExecuteProfiles(Parent, SessionKey, Remote, LocalDirectory, RemoteDirectory, AddMode,
        LocalDirectories, RemoteDirectories);
    }
    else
    {
      // TCustomScpExplorerForm::DoOpenDirectoryDialog
      QString & Directory = Remote ? RemoteDirectory : LocalDirectory;
      bool Continue = !AddMode || OpenDirectoryDialog::ConfirmAddBookmark(Parent, Directory);
      Result = false;
      if (Continue)
      {
        QString Name = Directory;
        // do not allow switching to location profiles, if we are not connected
        Result = OpenDirectoryDialog::Execute(Parent,
          AddMode ? OpenDirectoryDialog::odAddBookmark : OpenDirectoryDialog::odBrowse, Remote, Name,
          Remote ? RemoteDirectories : LocalDirectories, SessionKey, !SessionKey.isEmpty());
        if (Result)
        {
          Directory = Name;
        }
      }
      else if (First)
      {
        return false;
      }
    }
    // for second and further rounds, always do browse only
    AddMode = false;
    First = false;
  }
  while (!Result && (UseLocationProfiles != Config.UseLocationProfiles));
  return Result;
}

bool LocationProfilesDialog::ExecuteProfiles(QWidget * Parent, const QString & SessionKey, bool Remote,
  QString & LocalDirectory, QString & RemoteDirectory, bool AddMode,
  const QStringList & LocalDirectories, const QStringList & RemoteDirectories, bool * Switched)
{
  LocationProfilesDialog Dialog(Parent);
  Private * D = Dialog.d;
  D->SessionKey = SessionKey;
  D->RemoteSide = Remote;
  D->AddMode = AddMode;

  Bookmarks All;
  All.Load(Configuration::Instance().Ini());
  D->Pages[0].List = All.List(SessionKey);
  D->Pages[1].List = All.SharedList();
  D->Changing = true;
  D->LocalDirectoryEdit->addItems(LocalDirectories);
  D->RemoteDirectoryEdit->addItems(RemoteDirectories);
  D->LocalDirectoryEdit->setEditText(LocalDirectory);
  D->RemoteDirectoryEdit->setEditText(RemoteDirectory);
  D->Changing = false;
  D->Rebuild(D->Pages[0]);
  D->Rebuild(D->Pages[1]);
  D->PageControl->setCurrentIndex(Bookmarks::UseSharedBookmarks(Configuration::Instance().Ini()) ? 1 : 0);
  if (!AddMode)
  {
    D->FindProfile();
  }
  QComboBox * Active = Remote ? D->RemoteDirectoryEdit : D->LocalDirectoryEdit;
  Active->setFocus();
  Active->lineEdit()->selectAll();
  D->UpdateControls();

  bool Result = (Dialog.exec() == QDialog::Accepted);
  if (Result)
  {
    LocalDirectory = D->LocalDirectory();
    RemoteDirectory = D->RemoteDirectory();
  }
  if (Switched != nullptr)
  {
    *Switched = D->Switched;
  }
  return Result;
}

//---------------------------------------------------------------------------
// Shared bookmark shortcuts (TCustomScpExplorerForm::KeyDown)
//---------------------------------------------------------------------------

namespace
{

struct BookmarkShortCutWindow
{
  QPointer<QWidget> Window;
  std::function<void(const Bookmark &)> Open;
  QList<QPointer<QAction>> Actions;
};

QList<BookmarkShortCutWindow> & BookmarkShortCutWindows()
{
  static QList<BookmarkShortCutWindow> Windows;
  return Windows;
}

void UpdateWindowShortCuts(BookmarkShortCutWindow & Entry)
{
  for (const QPointer<QAction> & Action : Entry.Actions)
  {
    delete Action.data();
  }
  Entry.Actions.clear();
  if (!Entry.Window)
  {
    return;
  }
  QList<QKeySequence> Taken;
  for (QAction * Action : Entry.Window->findChildren<QAction *>())
  {
    Taken.append(Action->shortcuts());
  }
  BookmarkList Shared = Bookmarks::ConfiguredSharedList();
  for (const Bookmark & B : Shared.Items())
  {
    QKeySequence Sequence = LocationProfilesDialog::ShortCutToKeySequence(B.ShortCut);
    if (Sequence.isEmpty() || Taken.contains(Sequence))
    {
      continue;
    }
    Taken.append(Sequence);
    QAction * Action = new QAction(B.Name, Entry.Window);
    Action->setObjectName(QStringLiteral("BookmarkShortCutAction"));
    Action->setShortcut(Sequence);
    Action->setShortcutContext(Qt::WindowShortcut);
    std::function<void(const Bookmark &)> Open = Entry.Open;
    QObject::connect(Action, &QAction::triggered, Action, [Open, B]()
    {
      if (Open)
      {
        Open(B);
      }
    });
    Entry.Window->addAction(Action);
    Entry.Actions.append(Action);
  }
}

} // namespace

void LocationProfilesDialog::InstallBookmarkShortCuts(QWidget * Window, std::function<void(const Bookmark &)> Open)
{
  QList<BookmarkShortCutWindow> & Windows = BookmarkShortCutWindows();
  Windows.erase(std::remove_if(Windows.begin(), Windows.end(),
    [](const BookmarkShortCutWindow & Entry) { return Entry.Window.isNull(); }), Windows.end());
  for (BookmarkShortCutWindow & Entry : Windows)
  {
    if (Entry.Window == Window)
    {
      Entry.Open = std::move(Open);
      UpdateWindowShortCuts(Entry);
      return;
    }
  }
  BookmarkShortCutWindow Entry;
  Entry.Window = Window;
  Entry.Open = std::move(Open);
  UpdateWindowShortCuts(Entry);
  Windows.append(Entry);
}

void LocationProfilesDialog::RefreshBookmarkShortCuts()
{
  for (BookmarkShortCutWindow & Entry : BookmarkShortCutWindows())
  {
    UpdateWindowShortCuts(Entry);
  }
}
