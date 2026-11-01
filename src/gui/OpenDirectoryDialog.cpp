// Port of WinSCP source/forms/OpenDirectory.{cpp,dfm} and the add-bookmark
// confirmation of forms/CustomScpExplorer.cpp (DoOpenDirectoryDialog).
// Linux adaptation: local directories are compared case sensitively (WinSCP
// compares Windows paths case insensitively).
#include "gui/OpenDirectoryDialog.h"
#include "gui/GuiUserInterface.h"
#include "gui/HelpSystem.h"
#include "gui/LocationProfilesDialog.h"

#include "core/Bookmarks.h"
#include "core/Configuration.h"
#include "core/CustomCommands.h"
#include "core/FileSystem.h"

#include <QAbstractItemModel>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QStyle>
#include <QTabWidget>
#include <QVBoxLayout>

enum { NodeRole = Qt::UserRole, NameRole };

struct OpenDirectoryDialog::Private
{
  struct Page
  {
    bool Shared = false;
    QWidget * Sheet = nullptr;
    QListWidget * List = nullptr;
    BookmarkList Bookmarks;
    bool Modified = false;
    QPushButton * AddButton = nullptr;
    QPushButton * RemoveButton = nullptr;
    QPushButton * ShortCutButton = nullptr;
    QPushButton * UpButton = nullptr;
    QPushButton * DownButton = nullptr;
  };

  OpenDirectoryDialog * Q = nullptr;
  Mode DialogMode = odBrowse;
  bool Remote = false;
  QString SessionKey;
  bool AllowSwitch = false;
  bool Switched = false;
  bool Changing = false;
  bool BookmarkSelected = false;
  QComboBox * DirectoryEdit = nullptr;
  QPushButton * BrowseButton = nullptr;
  QPushButton * SwitchButton = nullptr;
  QPushButton * OkButton = nullptr;
  QTabWidget * PageControl = nullptr;
  Page Pages[2];   // 0 = site, 1 = shared

  Page & ActivePage() { return Pages[(PageControl->currentIndex() == 1) ? 1 : 0]; }

  QString Directory() const
  {
    // UnixExcludeTrailingBackslash / ExcludeTrailingBackslash
    QString Result = DirectoryEdit->currentText();
    while ((Result.size() > 1) && Result.endsWith(QLatin1Char('/')))
    {
      Result.chop(1);
    }
    return Result;
  }

  // TOpenDirectoryDialog::BookmarkDirectory / BookmarkText
  QString BookmarkDirectory(const Bookmark & B) const
  {
    return B.SideDirectory(Remote);
  }

  QString BookmarkText(const Bookmark & B) const
  {
    QString Result = BookmarkDirectory(B);
    if (!Result.isEmpty() && (B.ShortCut != 0))
    {
      Result = QStringLiteral("%1 (%2)").arg(Result, LocationProfilesDialog::ShortCutToText(B.ShortCut));
    }
    return Result;
  }

  // Index of the row's bookmark in the page's list
  int BookmarkIndex(const Page & P, int Row) const
  {
    QListWidgetItem * Item = P.List->item(Row);
    if (Item == nullptr)
    {
      return -1;
    }
    return P.Bookmarks.IndexOf(Item->data(NodeRole).toString(), Item->data(NameRole).toString());
  }

  QListWidgetItem * CreateItem(const Bookmark & B) const
  {
    QListWidgetItem * Item = new QListWidgetItem(BookmarkText(B));
    Item->setData(NodeRole, B.Node);
    Item->setData(NameRole, B.Name);
    Item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsDragEnabled);
    return Item;
  }

  // TOpenDirectoryDialog::FindBookmark
  int FindBookmark(const Page & P, const QString & Dir) const
  {
    for (int Row = 0; Row < P.List->count(); Row++)
    {
      int Index = BookmarkIndex(P, Row);
      if ((Index >= 0) && (BookmarkDirectory(P.Bookmarks.At(Index)) == Dir))
      {
        return Row;
      }
    }
    return -1;
  }

  // TOpenDirectoryDialog::LoadBookmarks
  void LoadBookmarks(Page & P)
  {
    P.List->clear();
    for (const Bookmark & B : P.Bookmarks.Items())
    {
      QString Dir = BookmarkDirectory(B);
      if (!Dir.isEmpty() && (FindBookmark(P, Dir) < 0))
      {
        P.List->addItem(CreateItem(B));
      }
    }
  }

  void UpdateBookmarkControls(Page & P)
  {
    int Row = P.List->currentRow();
    if ((Row >= 0) && !P.List->currentItem()->isSelected())
    {
      Row = -1;
    }
    P.AddButton->setEnabled(!Directory().isEmpty() && (FindBookmark(P, Directory()) < 0));
    P.RemoveButton->setEnabled(Row >= 0);
    if (P.ShortCutButton != nullptr)
    {
      P.ShortCutButton->setEnabled(Row >= 0);
    }
    P.UpButton->setEnabled(Row > 0);
    P.DownButton->setEnabled((Row >= 0) && (Row < P.List->count() - 1));
  }

  // TOpenDirectoryDialog::UpdateControls
  void UpdateControls()
  {
    OkButton->setEnabled(!Directory().isEmpty());
    BrowseButton->setVisible(!Remote);
    SwitchButton->setVisible(AllowSwitch);
    PageControl->setTabVisible(0, !SessionKey.isEmpty());
    UpdateBookmarkControls(Pages[0]);
    UpdateBookmarkControls(Pages[1]);
  }

  void SetDirectory(const QString & Value)
  {
    DirectoryEdit->setEditText(Value);
    DirectoryEditChange();
  }

  // TOpenDirectoryDialog::SelectBookmark
  void SelectBookmark(Page & P)
  {
    int Row = FindBookmark(P, Directory());
    if (Row >= 0)
    {
      bool PrevChanging = Changing;
      Changing = true;
      P.List->setCurrentRow(Row);
      Changing = PrevChanging;
    }
  }

  // TOpenDirectoryDialog::DirectoryEditChange
  void DirectoryEditChange()
  {
    SelectBookmark(Pages[0]);
    SelectBookmark(Pages[1]);
    UpdateControls();
    BookmarkSelected = false;
  }

  // TOpenDirectoryDialog::BookmarkSelected
  void SelectedBookmark(Page & P)
  {
    int Index = BookmarkIndex(P, P.List->currentRow());
    if (Index >= 0)
    {
      bool PrevChanging = Changing;
      Changing = true;
      SetDirectory(BookmarkDirectory(P.Bookmarks.At(Index)));
      Changing = PrevChanging;
    }
    UpdateControls();
    BookmarkSelected = true;
  }

  // TOpenDirectoryDialog::AddAsBookmark
  void AddAsBookmark(Page & P)
  {
    QString Dir = Directory();
    if (Dir.isEmpty() || (FindBookmark(P, Dir) >= 0))
    {
      return;
    }
    Bookmark B;
    if (Remote)
    {
      B.Remote = Dir;
    }
    else
    {
      B.Local = Dir;
    }
    B.Name = Dir;
    DirectoryEdit->lineEdit()->selectAll();

    int Row = P.List->currentRow();
    try
    {
      if ((Row >= 0) && P.List->currentItem()->isSelected())
      {
        P.Bookmarks.Insert(BookmarkIndex(P, Row), B);
        P.List->insertItem(Row, CreateItem(B));
        P.List->setCurrentRow(Row);
      }
      else
      {
        P.Bookmarks.Add(B);
        P.List->addItem(CreateItem(B));
        P.List->setCurrentRow(P.List->count() - 1);
      }
    }
    catch (const ExtException & E)
    {
      ShowErrorMessage(Q, E.Message());
      return;
    }
    P.Modified = true;
    UpdateControls();
  }

  // TOpenDirectoryDialog::RemoveBookmark
  void RemoveBookmark(Page & P)
  {
    int Row = P.List->currentRow();
    int Index = BookmarkIndex(P, Row);
    if (Index < 0)
    {
      return;
    }
    P.Bookmarks.Delete(Index);
    delete P.List->takeItem(Row);
    P.Modified = true;
    if (Row < P.List->count())
    {
      P.List->setCurrentRow(Row);
    }
    else
    {
      P.List->setCurrentRow(P.List->count() - 1);
    }
    UpdateControls();
    SelectedBookmark(P);
  }

  // TOpenDirectoryDialog::BookmarkMove (list rows); the bookmark list follows
  // TBookmarkList::MoveTo semantics (before the target when moving up, after it when moving down)
  void BookmarkMove(Page & P, int Source, int Dest)
  {
    if ((Source < 0) || (Source >= P.List->count()) || (Dest < 0) || (Dest >= P.List->count()) || (Source == Dest))
    {
      return;
    }
    int SourceIndex = BookmarkIndex(P, Source);
    int DestIndex = BookmarkIndex(P, Dest);
    if ((SourceIndex < 0) || (DestIndex < 0))
    {
      return;
    }
    P.Bookmarks.Move(SourceIndex, DestIndex);
    bool PrevChanging = Changing;
    Changing = true;
    QListWidgetItem * Item = P.List->takeItem(Source);
    P.List->insertItem(Dest, Item);
    P.List->setCurrentRow(Dest);
    Changing = PrevChanging;
    P.List->setFocus();
    P.Modified = true;
    UpdateControls();
  }

  // Drag & drop reordering (QListWidget InternalMove already moved the row)
  void RowMoved(Page & P, int OldRow, int NewRow)
  {
    if (OldRow == NewRow)
    {
      return;
    }
    QListWidgetItem * Moved = P.List->item(NewRow);
    QListWidgetItem * Target = P.List->item((NewRow > OldRow) ? (NewRow - 1) : (NewRow + 1));
    if ((Moved == nullptr) || (Target == nullptr))
    {
      return;
    }
    int SourceIndex = P.Bookmarks.IndexOf(Moved->data(NodeRole).toString(), Moved->data(NameRole).toString());
    int DestIndex = P.Bookmarks.IndexOf(Target->data(NodeRole).toString(), Target->data(NameRole).toString());
    if ((SourceIndex >= 0) && (DestIndex >= 0))
    {
      P.Bookmarks.Move(SourceIndex, DestIndex);
      P.Modified = true;
    }
    UpdateControls();
  }

  // TOpenDirectoryDialog::ShortCutBookmarkButtonClick
  void ShortCutBookmark(Page & P)
  {
    int Row = P.List->currentRow();
    int Index = BookmarkIndex(P, Row);
    if (Index < 0)
    {
      return;
    }
    Bookmark B = P.Bookmarks.At(Index);
    QList<int> Used = CustomCommandList::Configured().ShortCuts() + P.Bookmarks.ShortCuts();
    Used.removeAll(B.ShortCut);
    int ShortCut = B.ShortCut;
    if (LocationProfilesDialog::SelectShortCut(Q, ShortCut, Used))
    {
      B.ShortCut = ShortCut;
      P.Bookmarks.Update(Index, B);
      P.List->item(Row)->setText(BookmarkText(B));
      P.Modified = true;
      UpdateControls();
    }
  }

  void CreatePage(int Index, const QString & Caption)
  {
    Page & P = Pages[Index];
    P.Shared = (Index == 1);
    P.Sheet = new QWidget(PageControl);
    P.Sheet->setObjectName(P.Shared ? QStringLiteral("SharedBookmarksSheet") : QStringLiteral("SessionBookmarksSheet"));
    QHBoxLayout * Layout = new QHBoxLayout(P.Sheet);
    P.List = new QListWidget(P.Sheet);
    P.List->setObjectName(P.Shared ? QStringLiteral("SharedBookmarksList") : QStringLiteral("SessionBookmarksList"));
    P.List->setSelectionMode(QAbstractItemView::SingleSelection);
    P.List->setDragDropMode(QAbstractItemView::InternalMove);
    P.List->setDefaultDropAction(Qt::MoveAction);
    P.List->installEventFilter(Q);
    Layout->addWidget(P.List, 1);
    QVBoxLayout * Buttons = new QVBoxLayout();
    P.AddButton = new QPushButton(QStringLiteral("&Add"), P.Sheet);
    P.RemoveButton = new QPushButton(QStringLiteral("Remo&ve"), P.Sheet);
    P.UpButton = new QPushButton(QStringLiteral("&Up"), P.Sheet);
    P.DownButton = new QPushButton(QStringLiteral("&Down"), P.Sheet);
    Buttons->addWidget(P.AddButton);
    Buttons->addWidget(P.RemoveButton);
    if (P.Shared)
    {
      P.ShortCutButton = new QPushButton(QStringLiteral("&Shortcut..."), P.Sheet);
      Buttons->addWidget(P.ShortCutButton);
      QObject::connect(P.ShortCutButton, &QPushButton::clicked, Q, [this, Index]() { ShortCutBookmark(Pages[Index]); });
    }
    Buttons->addStretch(1);
    Buttons->addWidget(P.UpButton);
    Buttons->addWidget(P.DownButton);
    for (QPushButton * Button : { P.AddButton, P.RemoveButton, P.ShortCutButton, P.UpButton, P.DownButton })
    {
      if (Button != nullptr)
      {
        Button->setAutoDefault(false);
      }
    }
    Layout->addLayout(Buttons);
    PageControl->addTab(P.Sheet, Caption);

    QObject::connect(P.AddButton, &QPushButton::clicked, Q, [this, Index]() { AddAsBookmark(Pages[Index]); });
    QObject::connect(P.RemoveButton, &QPushButton::clicked, Q, [this, Index]() { RemoveBookmark(Pages[Index]); });
    QObject::connect(P.UpButton, &QPushButton::clicked, Q, [this, Index]()
    {
      Page & AP = Pages[Index];
      BookmarkMove(AP, AP.List->currentRow(), AP.List->currentRow() - 1);
    });
    QObject::connect(P.DownButton, &QPushButton::clicked, Q, [this, Index]()
    {
      Page & AP = Pages[Index];
      BookmarkMove(AP, AP.List->currentRow(), AP.List->currentRow() + 1);
    });
    // BookmarksListClick (keyboard selection too)
    QObject::connect(P.List, &QListWidget::currentRowChanged, Q, [this, Index](int)
    {
      if (!Changing)
      {
        SelectedBookmark(Pages[Index]);
      }
      else
      {
        UpdateControls();
      }
    });
    QObject::connect(P.List, &QListWidget::itemClicked, Q, [this, Index](QListWidgetItem *)
    {
      SelectedBookmark(Pages[Index]);
    });
    // BookmarksListDblClick
    QObject::connect(P.List, &QListWidget::itemDoubleClicked, Q, [this](QListWidgetItem *)
    {
      if (OkButton->isEnabled())
      {
        Q->accept();
      }
    });
    QObject::connect(P.List->model(), &QAbstractItemModel::rowsMoved, Q,
      [this, Index](const QModelIndex &, int Start, int, const QModelIndex &, int Row)
      {
        if (!Changing)
        {
          RowMoved(Pages[Index], Start, (Row > Start) ? (Row - 1) : Row);
        }
      });
  }
};

OpenDirectoryDialog::OpenDirectoryDialog(QWidget * Parent, Mode AMode, bool Remote, const QString & SessionKey, bool AllowSwitch) :
  QDialog(Parent),
  d(new Private)
{
  d->Q = this;
  d->DialogMode = AMode;
  d->Remote = Remote;
  d->SessionKey = SessionKey;
  d->AllowSwitch = AllowSwitch && !SessionKey.isEmpty();
  setObjectName(QStringLiteral("OpenDirectoryDialog"));
  // SetMode: OPEN_DIRECTORY_BROWSE_CAPTION / OPEN_DIRECTORY_ADD_BOOMARK_ACTION
  setWindowTitle((AMode == odBrowse) ? QStringLiteral("Open directory") : QStringLiteral("Manage bookmarks"));
  setSizeGripEnabled(true);

  QGridLayout * Layout = new QGridLayout(this);
  QLabel * Image = new QLabel(this);
  Image->setPixmap(style()->standardIcon(QStyle::SP_DirOpenIcon).pixmap(32, 32));
  Image->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
  Layout->addWidget(Image, 0, 0, 2, 1, Qt::AlignTop);

  d->DirectoryEdit = new QComboBox(this);
  d->DirectoryEdit->setObjectName(Remote ? QStringLiteral("RemoteDirectoryEdit") : QStringLiteral("LocalDirectoryEdit"));
  d->DirectoryEdit->setEditable(true);
  d->DirectoryEdit->setInsertPolicy(QComboBox::NoInsert);
  d->DirectoryEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  d->DirectoryEdit->setMinimumContentsLength(30);
  d->DirectoryEdit->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  if (Remote)
  {
    d->DirectoryEdit->lineEdit()->setMaxLength(1000);
  }
  QLabel * EditLabel = new QLabel(QStringLiteral("&Open directory:"), this);
  EditLabel->setBuddy(d->DirectoryEdit);
  d->BrowseButton = new QPushButton(QStringLiteral("B&rowse..."), this);
  d->BrowseButton->setAutoDefault(false);
  Layout->addWidget(EditLabel, 0, 1, 1, 2);
  Layout->addWidget(d->DirectoryEdit, 1, 1);
  Layout->addWidget(d->BrowseButton, 1, 2);

  d->PageControl = new QTabWidget(this);
  d->CreatePage(0, QStringLiteral("Site bookmarks"));
  d->CreatePage(1, QStringLiteral("Shared bookmarks"));
  Layout->addWidget(d->PageControl, 2, 0, 1, 3);
  Layout->setRowStretch(2, 1);
  Layout->setColumnStretch(1, 1);

  QDialogButtonBox * Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  d->OkButton = Buttons->button(QDialogButtonBox::Ok);
  d->SwitchButton = Buttons->addButton(QStringLiteral("&Location Profiles..."), QDialogButtonBox::ResetRole);
  d->SwitchButton->setAutoDefault(false);
  HelpSystem::AddHelpButton(Buttons, this, QStringLiteral("ui_opendir"));
  Layout->addWidget(Buttons, 3, 0, 1, 3);
  connect(Buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  // SwitchButtonClick (ModalResult = mrCancel)
  connect(d->SwitchButton, &QPushButton::clicked, this, [this]()
  {
    d->Switched = true;
    Configuration::Instance().UseLocationProfiles = true;
    reject();
  });
  // LocalDirectoryBrowseButtonClick
  connect(d->BrowseButton, &QPushButton::clicked, this, [this]()
  {
    QString Dir = QFileDialog::getExistingDirectory(this, QString(), d->Directory());
    if (!Dir.isEmpty())
    {
      d->SetDirectory(Dir);
    }
  });
  connect(d->DirectoryEdit, &QComboBox::editTextChanged, this, [this]()
  {
    if (!d->Changing)
    {
      d->DirectoryEditChange();
    }
  });
  // PageControlChange
  connect(d->PageControl, &QTabWidget::currentChanged, this, [this](int)
  {
    if (!d->Changing)
    {
      d->SelectedBookmark(d->ActivePage());
    }
  });
  resize(450, 342);
}

OpenDirectoryDialog::~OpenDirectoryDialog()
{
  delete d;
}

QString OpenDirectoryDialog::Directory() const
{
  return d->Directory();
}

void OpenDirectoryDialog::SetDirectory(const QString & Value)
{
  d->SetDirectory(Value);
}

void OpenDirectoryDialog::SetDirectories(const QStringList & Directories)
{
  QString Text = d->DirectoryEdit->currentText();
  bool PrevChanging = d->Changing;
  d->Changing = true;
  d->DirectoryEdit->clear();
  d->DirectoryEdit->addItems(Directories);
  d->DirectoryEdit->setEditText(Text);
  d->Changing = PrevChanging;
}

bool OpenDirectoryDialog::Switched() const
{
  return d->Switched;
}

QTabWidget * OpenDirectoryDialog::PageControl() const
{
  return d->PageControl;
}

QListWidget * OpenDirectoryDialog::BookmarksList(bool Shared) const
{
  return d->Pages[Shared ? 1 : 0].List;
}

QPushButton * OpenDirectoryDialog::Button(bool Shared, const QString & Name) const
{
  const Private::Page & P = d->Pages[Shared ? 1 : 0];
  if (Name == QStringLiteral("Add")) return P.AddButton;
  if (Name == QStringLiteral("Remove")) return P.RemoveButton;
  if (Name == QStringLiteral("Up")) return P.UpButton;
  if (Name == QStringLiteral("Down")) return P.DownButton;
  if (Name == QStringLiteral("ShortCut")) return P.ShortCutButton;
  return nullptr;
}

QComboBox * OpenDirectoryDialog::DirectoryEdit() const
{
  return d->DirectoryEdit;
}

QPushButton * OpenDirectoryDialog::SwitchButton() const
{
  return d->SwitchButton;
}

void OpenDirectoryDialog::showEvent(QShowEvent * Event)
{
  QDialog::showEvent(Event);
  // FormShow
  d->UpdateControls();
  if (d->DialogMode == odBrowse)
  {
    d->DirectoryEdit->setFocus();
    d->DirectoryEdit->lineEdit()->selectAll();
  }
  else
  {
    d->ActivePage().List->setFocus();
  }
}

bool OpenDirectoryDialog::eventFilter(QObject * Watched, QEvent * Event)
{
  if (Event->type() == QEvent::KeyPress)
  {
    // BookmarksListKeyDown: Delete removes the bookmark
    QKeyEvent * KeyEvent = static_cast<QKeyEvent *>(Event);
    for (Private::Page & P : d->Pages)
    {
      if ((Watched == P.List) && (KeyEvent->key() == Qt::Key_Delete) && (P.List->currentRow() >= 0))
      {
        d->RemoveBookmark(P);
        return true;
      }
    }
  }
  return QDialog::eventFilter(Watched, Event);
}

void OpenDirectoryDialog::SaveBookmarks()
{
  // Execute: WinConfiguration->SharedBookmarks / Bookmarks[SessionKey] / UseSharedBookmarks
  IniFile & Ini = Configuration::Instance().Ini();
  bool Changed = false;
  if (d->Pages[0].Modified || d->Pages[1].Modified)
  {
    Bookmarks All;
    All.Load(Ini);
    if (d->Pages[1].Modified)
    {
      All.SetSharedList(d->Pages[1].Bookmarks);
    }
    if (d->Pages[0].Modified && !d->SessionKey.isEmpty())
    {
      All.SetList(d->SessionKey, d->Pages[0].Bookmarks);
    }
    All.Save(Ini);
    d->Pages[0].Modified = false;
    d->Pages[1].Modified = false;
    Changed = true;
  }
  if (!d->SessionKey.isEmpty())
  {
    // Do not remember that shared page was selected, if it was selected
    // implicitly because there's no site page.
    bool UseShared = (d->PageControl->currentIndex() == 1);
    if (Bookmarks::UseSharedBookmarks(Ini) != UseShared)
    {
      Bookmarks::SetUseSharedBookmarks(Ini, UseShared);
      Changed = true;
    }
  }
  if (Changed || d->Switched)
  {
    Configuration::Instance().Save();
  }
  if (Changed)
  {
    LocationProfilesDialog::RefreshBookmarkShortCuts();
  }
}

bool OpenDirectoryDialog::ConfirmAddBookmark(QWidget * Parent, const QString & Directory)
{
  // TCustomScpExplorerForm::DoOpenDirectoryDialog (odAddBookmark)
  IniFile & Ini = Configuration::Instance().Ini();
  MessageParams Params;
  Params.Params = qpNeverAskAgainCheck;
  Params.NeverAskAgainTitle = QStringLiteral("Add to &shared bookmarks");   // ADD_BOOKMARK_SHARED
  Params.NeverAskAgainCheckedInitially = Bookmarks::UseSharedBookmarks(Ini);
  unsigned int Answer = MessageDialog(Parent,
    QStringLiteral("**Add directory '%1' to bookmarks?**").arg(Directory),   // ADD_BOOKMARK_CONFIRM
    qtConfirmation, qaOK | qaCancel, QStringLiteral("task_navigate#bookmarks"), &Params);
  bool Result;
  if (Answer == qaNeverAskAgain)
  {
    Bookmarks::SetUseSharedBookmarks(Ini, true);
    Result = true;
  }
  else if (Answer == qaOK)
  {
    Bookmarks::SetUseSharedBookmarks(Ini, false);
    Result = true;
  }
  else
  {
    Result = false;
  }
  if (Result)
  {
    Configuration::Instance().Save();
  }
  return Result;
}

bool OpenDirectoryDialog::Execute(QWidget * Parent, Mode AMode, bool Remote, QString & Directory,
  const QStringList & Directories, const QString & SessionKey, bool AllowSwitch)
{
  OpenDirectoryDialog Dialog(Parent, AMode, Remote, SessionKey, AllowSwitch);
  Private * D = Dialog.d;
  Bookmarks All;
  All.Load(Configuration::Instance().Ini());
  if (!SessionKey.isEmpty())
  {
    D->Pages[0].Bookmarks = All.List(SessionKey);
    D->LoadBookmarks(D->Pages[0]);
  }
  D->Pages[1].Bookmarks = All.SharedList();
  D->LoadBookmarks(D->Pages[1]);
  Dialog.SetDirectories(Directories);
  D->Changing = true;
  D->PageControl->setCurrentIndex(
    (SessionKey.isEmpty() || Bookmarks::UseSharedBookmarks(Configuration::Instance().Ini())) ? 1 : 0);
  D->Changing = false;
  D->SetDirectory(Directory);
  if (AMode == odAddBookmark)
  {
    D->AddAsBookmark(D->ActivePage());
  }
  D->BookmarkSelected = false;
  bool Result = (Dialog.exec() == QDialog::Accepted);
  Dialog.SaveBookmarks();
  if (Result)
  {
    Directory = Dialog.Directory();
  }
  return Result;
}
