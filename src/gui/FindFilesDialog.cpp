// Port of WinSCP source/forms/FileFind.{cpp,dfm}
// (animation, mask hints balloon and the minimized-window notification are not ported)
#include "gui/FindFilesDialog.h"
#include "gui/EditMaskDialog.h"
#include "gui/HelpSystem.h"
#include "gui/HistoryComboBox.h"
#include "gui/FileListModel.h"

#include "core/Configuration.h"
#include "core/FileFind.h"
#include "core/IniStorage.h"
#include "core/Terminal.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QFileIconProvider>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMetaMethod>
#include <QPointer>
#include <QPushButton>
#include <QStatusBar>
#include <QToolTip>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace
{

enum Columns { ColName, ColDirectory, ColSize, ColChanged };

class FoundItem : public QTreeWidgetItem
{
public:
  explicit FoundItem(const FoundFile & AFile) : File(AFile) {}
  FoundFile File;

  // TFileFindDialog::FilesCompare
  bool operator<(const QTreeWidgetItem & Other) const override
  {
    const FoundFile & Other2 = static_cast<const FoundItem &>(Other).File;
    int Column = (treeWidget() != nullptr) ? treeWidget()->sortColumn() : ColName;
    int Result = 0;
    switch (Column)
    {
      case ColName:
        Result = File.File.FileName.compare(Other2.File.FileName, Qt::CaseInsensitive);
        break;
      case ColSize:
        Result = (File.File.Size < Other2.File.Size) ? -1 : ((File.File.Size > Other2.File.Size) ? 1 : 0);
        break;
      case ColChanged:
        Result = (File.File.Modification < Other2.File.Modification) ? -1 :
          ((File.File.Modification > Other2.File.Modification) ? 1 : 0);
        break;
      default:
        break;
    }
    if (Result == 0)
    {
      Result = File.Directory.compare(Other2.Directory, Qt::CaseInsensitive);
      if (Result == 0)
      {
        Result = File.File.FileName.compare(Other2.File.FileName, Qt::CaseInsensitive);
      }
    }
    return Result < 0;
  }
};

} // namespace

struct FindFilesDialog::Private : public QObject
{
  enum State { ffInit, ffFinding, ffAborting, ffDone, ffAborted };

  FindFilesDialog * Q = nullptr;
  QPointer<Terminal> Term;
  bool Local = true;
  QString TerminalName;
  FileFind * Finder = nullptr;
  State FState = ffInit;
  bool ClosePending = false;
  QString FocusPath;            // focus requested while finding
  QString Directory;            // search root (for the "Directory" column)
  QString FindingInDirectory;
  bool Sorted = false;

  QGroupBox * FilterGroup = nullptr;
  QComboBox * MaskEdit = nullptr;
  QComboBox * DirectoryEdit = nullptr;
  QPushButton * MaskButton = nullptr;
  QPushButton * StartStopButton = nullptr;
  QPushButton * FocusButton = nullptr;
  QPushButton * EditButton = nullptr;
  QPushButton * DownloadButton = nullptr;
  QPushButton * DeleteButton = nullptr;
  QPushButton * CopyButton = nullptr;
  QTreeWidget * FileView = nullptr;
  QStatusBar * StatusBar = nullptr;
  QAction * FocusAction = nullptr;
  QAction * EditAction = nullptr;
  QAction * DownloadAction = nullptr;
  QAction * DeleteAction = nullptr;
  QAction * SelectAllAction = nullptr;
  QAction * CopyAction = nullptr;
  QFileIconProvider IconProvider;

  bool IsFinding() const { return (FState == ffFinding) || (FState == ffAborting); }

  QString DirectoryHistoryKey() const
  {
    return Local ? QStringLiteral("LocalDirectory") : QStringLiteral("RemoteDirectory");
  }

  QList<FoundItem *> SelectedItems() const
  {
    QList<FoundItem *> Result;
    for (QTreeWidgetItem * Item : FileView->selectedItems())
    {
      Result.append(static_cast<FoundItem *>(Item));
    }
    return Result;
  }

  QStringList SelectedPaths() const
  {
    QStringList Result;
    for (int Index = 0; Index < FileView->topLevelItemCount(); Index++)
    {
      QTreeWidgetItem * Item = FileView->topLevelItem(Index);
      if (Item->isSelected())
      {
        Result.append(static_cast<FoundItem *>(Item)->File.FullPath);
      }
    }
    return Result;
  }

  void UpdateControls()
  {
    bool Finding = IsFinding();
    // FIND_FILE_FINDING / FIND_FILE_TITLE
    QString Caption = Finding ? QStringLiteral("Finding ...") : QStringLiteral("Find");
    if (!TerminalName.isEmpty())
    {
      Caption += QStringLiteral(" - ") + TerminalName;
    }
    Q->setWindowTitle(Caption);
    if (Finding)
    {
      StartStopButton->setEnabled(true);
      StartStopButton->setText(QStringLiteral("&Stop"));
    }
    else
    {
      StartStopButton->setEnabled(!DirectoryEdit->currentText().trimmed().isEmpty() && (Local || !Term.isNull()));
      StartStopButton->setText(QStringLiteral("&Start"));
    }
    FilterGroup->setEnabled(!Finding);
    int SelCount = FileView->selectedItems().size();
    int Count = FileView->topLevelItemCount();
    FocusAction->setEnabled(FileView->currentItem() != nullptr);
    bool EnableFileOperations = !Finding && (SelCount > 0) && (Local || !Term.isNull());
    DeleteAction->setEnabled(EnableFileOperations);
    DownloadAction->setEnabled(EnableFileOperations);
    EditAction->setEnabled(EnableFileOperations);
    CopyAction->setEnabled(Count > 0);
    SelectAllAction->setEnabled(SelCount < Count);
    FocusButton->setEnabled(FocusAction->isEnabled());
    EditButton->setEnabled(EditAction->isEnabled());
    DownloadButton->setEnabled(DownloadAction->isEnabled());
    DeleteButton->setEnabled(DeleteAction->isEnabled());
    CopyButton->setEnabled(CopyAction->isEnabled());

    switch (FState)
    {
      case ffInit:
        StatusBar->clearMessage();
        break;
      case ffFinding:
      case ffAborting:
        if (!FindingInDirectory.isEmpty())
        {
          StatusBar->showMessage(QStringLiteral("Searching in %1").arg(FindingInDirectory));   // FIND_FILE_IN_DIRECTORY
        }
        else
        {
          StatusBar->clearMessage();
        }
        break;
      case ffAborted:
        StatusBar->showMessage(QStringLiteral("Aborted."));   // FIND_FILE_ABORTED
        break;
      case ffDone:
        StatusBar->showMessage(QStringLiteral("Done."));      // FIND_FILE_DONE
        break;
    }
    bool FocusDefault = FileView->hasFocus() && (FState != ffInit);
    FocusButton->setDefault(FocusDefault);
    StartStopButton->setDefault(!FocusDefault);
  }

  void Clear()
  {
    FileView->clear();
  }

  void Start()
  {
    QString Mask = MaskEdit->currentText();
    if (!EditMaskDialog::ValidateMask(Q, Mask, MaskEdit))
    {
      return;
    }
    QString Dir = DirectoryEdit->currentText().trimmed();
    if (Local)
    {
      if ((Dir == QStringLiteral("~")) || Dir.startsWith(QStringLiteral("~/")))
      {
        Dir = QDir::homePath() + Dir.mid(1);
      }
      Dir = QDir::cleanPath(QDir(Dir).absolutePath());
    }
    // UnixExcludeTrailingBackslash
    while ((Dir.size() > 1) && Dir.endsWith(QLatin1Char('/')))
    {
      Dir.chop(1);
    }
    HistoryComboBox::SaveToHistory(DirectoryHistoryKey(), Dir);
    HistoryComboBox::SaveToHistory(QStringLiteral("Mask"), Mask);
    HistoryComboBox::LoadHistoryCombo(DirectoryEdit, DirectoryHistoryKey());
    HistoryComboBox::LoadHistoryCombo(MaskEdit, QStringLiteral("Mask"));
    EditMaskDialog::SetSelectMask(Mask);

    Directory = (Dir == QStringLiteral("/")) ? QString() : Dir;
    Clear();
    QString Error;
    if (!Finder->Start(Local ? nullptr : Term.data(), Dir, Mask, &Error))
    {
      QMessageBox::critical(Q, QStringLiteral("Error"), Error);
      return;
    }
    FState = ffFinding;
    ClosePending = false;
    FindingInDirectory.clear();
    UpdateControls();
  }

  void Stop()
  {
    FState = ffAborting;
    Finder->Stop();
    UpdateControls();
  }

  bool StopIfFinding()
  {
    bool Result = IsFinding();
    if (Result)
    {
      Stop();
    }
    return Result;
  }

  void FilesFound(const QList<FoundFile> & Files)
  {
    QList<QTreeWidgetItem *> Items;
    for (const FoundFile & Found : Files)
    {
      // TFileFindDialog::FileFound
      FoundItem * Item = new FoundItem(Found);
      QString Caption = Found.File.FileName;
      if (Found.File.IsDirectory)
      {
        Caption += QLatin1Char('/');
      }
      Item->setText(ColName, Caption);
      Item->setIcon(ColName, IconProvider.icon(Found.File.IsDirectory ? QAbstractFileIconProvider::Folder : QAbstractFileIconProvider::File));
      QString Dir = Found.Directory;
      if (Dir == QStringLiteral("/"))
      {
        Dir.clear();
      }
      if (Dir.startsWith(Directory, Qt::CaseInsensitive) && ((Dir.size() == Directory.size()) || (Dir.at(Directory.size()) == QLatin1Char('/'))))
      {
        Dir = QLatin1Char('.') + Dir.mid(Directory.size());
      }
      Item->setText(ColDirectory, Dir);
      Item->setToolTip(ColDirectory, Found.Directory);
      Item->setText(ColSize, Found.File.IsDirectory ? QString() : FileListModel::FormatSizeKB(Found.File.Size));
      Item->setTextAlignment(ColSize, Qt::AlignRight | Qt::AlignVCenter);
      Item->setText(ColChanged, Found.File.Modification.isValid() ?
        QLocale().toString(Found.File.Modification.toLocalTime(), QLocale::ShortFormat) : QString());
      Items.append(Item);
    }
    FileView->addTopLevelItems(Items);
    if (Sorted)
    {
      FileView->sortItems(FileView->sortColumn(), FileView->header()->sortIndicatorOrder());
    }
    UpdateControls();
  }

  void Finished(bool Aborted, const QString & Error)
  {
    FindingInDirectory.clear();
    if (FState == ffFinding)
    {
      FState = Aborted ? ffAborted : ffDone;
    }
    else if (FState == ffAborting)
    {
      FState = ffAborted;
    }
    UpdateControls();
    if (!Error.isEmpty())
    {
      QMessageBox::critical(Q, QStringLiteral("Error"), Error);
    }
    if (!FocusPath.isEmpty())
    {
      QString Path = FocusPath;
      FocusPath.clear();
      DoFocusFile(Path);
    }
    if (ClosePending)
    {
      ClosePending = false;
      Q->close();
    }
  }

  void DoFocusFile(const QString & Path)
  {
    int P = Path.lastIndexOf(QLatin1Char('/'));
    QString Dir = (P <= 0) ? QStringLiteral("/") : Path.left(P);
    QString Name = Path.mid(P + 1);
    emit Q->FocusRequested(Dir, Name);
  }

  void FocusFile()
  {
    QTreeWidgetItem * Current = FileView->currentItem();
    if (Current == nullptr)
    {
      return;
    }
    QString Path = static_cast<FoundItem *>(Current)->File.FullPath;
    if (StopIfFinding())
    {
      FocusPath = Path;
    }
    else
    {
      DoFocusFile(Path);
    }
  }

  void CopyToClipboard()
  {
    QStringList Lines;
    for (int Index = 0; Index < FileView->topLevelItemCount(); Index++)
    {
      Lines.append(static_cast<FoundItem *>(FileView->topLevelItem(Index))->File.FullPath);
    }
    if (!Lines.isEmpty())
    {
      QApplication::clipboard()->setText(Lines.join(QLatin1Char('\n')) + QLatin1Char('\n'));
    }
  }

  QString ConfigKey() const { return QStringLiteral("FindFile"); }

  bool OpenConfig(IniStorage & Storage, bool CanCreate) const
  {
    return Storage.OpenSubKey(QStringLiteral("Configuration"), CanCreate) &&
      Storage.OpenSubKey(QStringLiteral("Interface"), CanCreate) &&
      Storage.OpenSubKey(QStringLiteral("NixSCP"), CanCreate) &&
      Storage.OpenSubKey(ConfigKey(), CanCreate);
  }

  void LoadState()
  {
    IniStorage Storage(&Configuration::Instance().Ini());
    if (OpenConfig(Storage, false))
    {
      QByteArray Geometry = QByteArray::fromHex(Storage.ReadStringRaw(QStringLiteral("WindowGeometry"), QString()).toLatin1());
      if (!Geometry.isEmpty())
      {
        Q->restoreGeometry(Geometry);
      }
      QByteArray Header = QByteArray::fromHex(Storage.ReadStringRaw(QStringLiteral("ListState"), QString()).toLatin1());
      if (!Header.isEmpty())
      {
        FileView->header()->restoreState(Header);
        // never start sorted (TFileFindDialog::FormShow, NoSort)
        FileView->header()->setSortIndicatorShown(false);
      }
    }
  }

  void SaveState()
  {
    IniStorage Storage(&Configuration::Instance().Ini());
    if (OpenConfig(Storage, true))
    {
      Storage.WriteStringRaw(QStringLiteral("WindowGeometry"), QString::fromLatin1(Q->saveGeometry().toHex()));
      Storage.WriteStringRaw(QStringLiteral("ListState"), QString::fromLatin1(FileView->header()->saveState().toHex()));
    }
  }
};

FindFilesDialog::FindFilesDialog(Terminal * Term, const QString & StartDirectory, QWidget * Parent) :
  QDialog(Parent, Qt::Window),
  d(new Private)
{
  d->Q = this;
  setAttribute(Qt::WA_DeleteOnClose);
  setSizeGripEnabled(true);
  d->Finder = new FileFind(d);
  connect(d->Finder, &FileFind::FilesFound, d, [this](const QList<FoundFile> & Files) { d->FilesFound(Files); });
  connect(d->Finder, &FileFind::SearchingDirectory, d, [this](const QString & Directory)
  {
    d->FindingInDirectory = Directory;
    d->UpdateControls();
  });
  connect(d->Finder, &FileFind::Finished, d, [this](bool Aborted, const QString & Error) { d->Finished(Aborted, Error); });

  QGridLayout * MainLayout = new QGridLayout(this);
  MainLayout->setContentsMargins(8, 8, 8, 0);

  // Filter group
  d->FilterGroup = new QGroupBox(QStringLiteral("Filter"), this);
  QGridLayout * FilterLayout = new QGridLayout(d->FilterGroup);
  d->MaskEdit = new QComboBox(d->FilterGroup);
  d->MaskEdit->setEditable(true);
  d->MaskEdit->setInsertPolicy(QComboBox::NoInsert);
  d->MaskEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  d->MaskEdit->setMinimumContentsLength(20);
  d->MaskEdit->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  QLabel * MaskLabel = new QLabel(QStringLiteral("&File mask:"), d->FilterGroup);
  MaskLabel->setBuddy(d->MaskEdit);
  QLabel * MaskHintText = new QLabel(QStringLiteral("<a href=\"#\">mask hints</a>"), d->FilterGroup);
  QString Hints = EditMaskDialog::MaskHints(true, true);
  MaskHintText->setToolTip(Hints);
  connect(MaskHintText, &QLabel::linkActivated, MaskHintText, [MaskHintText, Hints]()
  {
    QToolTip::showText(MaskHintText->mapToGlobal(QPoint(0, MaskHintText->height())), Hints, MaskHintText);
  });
  d->MaskButton = new QPushButton(QStringLiteral("&Edit..."), d->FilterGroup);
  d->DirectoryEdit = new QComboBox(d->FilterGroup);
  d->DirectoryEdit->setEditable(true);
  d->DirectoryEdit->setInsertPolicy(QComboBox::NoInsert);
  d->DirectoryEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  d->DirectoryEdit->setMinimumContentsLength(20);
  d->DirectoryEdit->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  QLabel * DirectoryLabel = new QLabel(QStringLiteral("Sear&ch in:"), d->FilterGroup);
  DirectoryLabel->setBuddy(d->DirectoryEdit);
  FilterLayout->addWidget(MaskLabel, 0, 0);
  FilterLayout->addWidget(MaskHintText, 0, 1, Qt::AlignRight);
  FilterLayout->addWidget(d->MaskEdit, 1, 0, 1, 2);
  FilterLayout->addWidget(d->MaskButton, 1, 2);
  FilterLayout->addWidget(DirectoryLabel, 2, 0);
  FilterLayout->addWidget(d->DirectoryEdit, 3, 0, 1, 3);
  FilterLayout->setColumnStretch(0, 1);
  MainLayout->addWidget(d->FilterGroup, 0, 0);

  // Results
  d->FileView = new QTreeWidget(this);
  d->FileView->setColumnCount(4);
  d->FileView->setHeaderLabels({ QStringLiteral("Name"), QStringLiteral("Directory"), QStringLiteral("Size"),
    QStringLiteral("Date modified") });
  d->FileView->setRootIsDecorated(false);
  d->FileView->setUniformRowHeights(true);
  d->FileView->setAllColumnsShowFocus(true);
  d->FileView->setSelectionMode(QAbstractItemView::ExtendedSelection);
  d->FileView->setContextMenuPolicy(Qt::CustomContextMenu);
  d->FileView->header()->setSectionsClickable(true);
  d->FileView->header()->setSortIndicatorShown(false);
  d->FileView->header()->setStretchLastSection(false);
  d->FileView->header()->resizeSection(ColName, 160);
  d->FileView->header()->resizeSection(ColDirectory, 180);
  d->FileView->header()->resizeSection(ColSize, 80);
  d->FileView->header()->resizeSection(ColChanged, 130);
  d->FileView->headerItem()->setTextAlignment(ColSize, Qt::AlignRight | Qt::AlignVCenter);
  MainLayout->addWidget(d->FileView, 1, 0);

  // Buttons
  d->StartStopButton = new QPushButton(QStringLiteral("&Start"), this);
  d->FocusButton = new QPushButton(QStringLiteral("Fo&cus"), this);
  d->EditButton = new QPushButton(QStringLiteral("Edi&t"), this);
  d->DownloadButton = new QPushButton(QStringLiteral("Down&load..."), this);
  d->DeleteButton = new QPushButton(QStringLiteral("&Delete"), this);
  d->CopyButton = new QPushButton(QStringLiteral("&Copy Results"), this);
  for (QPushButton * Button : { d->StartStopButton, d->FocusButton, d->EditButton, d->DownloadButton, d->DeleteButton, d->CopyButton })
  {
    Button->setAutoDefault(false);
    Button->setMinimumWidth(108);
  }
  QPushButton * HelpButton = new QPushButton(QStringLiteral("&Help"), this);
  HelpButton->setAutoDefault(false);
  HelpButton->setMinimumWidth(108);
  HelpSystem::InstallFormHelp(this, QStringLiteral("ui_find"), HelpButton);
  QVBoxLayout * TopButtons = new QVBoxLayout();
  TopButtons->addWidget(d->StartStopButton);
  TopButtons->addWidget(HelpButton);
  TopButtons->addStretch(1);
  MainLayout->addLayout(TopButtons, 0, 1);
  QVBoxLayout * FileButtons = new QVBoxLayout();
  FileButtons->addWidget(d->FocusButton);
  FileButtons->addWidget(d->EditButton);
  FileButtons->addWidget(d->DownloadButton);
  FileButtons->addWidget(d->DeleteButton);
  FileButtons->addStretch(1);
  FileButtons->addWidget(d->CopyButton);
  MainLayout->addLayout(FileButtons, 1, 1);

  d->StatusBar = new QStatusBar(this);
  d->StatusBar->setSizeGripEnabled(true);
  MainLayout->addWidget(d->StatusBar, 2, 0, 1, 2);
  MainLayout->setRowStretch(1, 1);
  MainLayout->setColumnStretch(0, 1);

  // Actions (ActionList of the form)
  d->FocusAction = new QAction(QStringLiteral("Fo&cus"), this);
  d->EditAction = new QAction(QStringLiteral("&Edit"), this);
  d->EditAction->setShortcuts({ QKeySequence(Qt::Key_F4), QKeySequence(Qt::CTRL | Qt::Key_E) });
  d->DownloadAction = new QAction(QStringLiteral("Down&load..."), this);
  d->DownloadAction->setShortcut(QKeySequence(Qt::Key_F5));
  d->DeleteAction = new QAction(QStringLiteral("&Delete"), this);
  d->DeleteAction->setShortcuts({ QKeySequence(Qt::Key_Delete), QKeySequence(Qt::Key_F8) });
  d->SelectAllAction = new QAction(QStringLiteral("Select &All"), this);
  d->SelectAllAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_A));
  d->CopyAction = new QAction(QStringLiteral("&Copy Results"), this);
  d->CopyAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_C));
  for (QAction * Action : { d->EditAction, d->DownloadAction, d->DeleteAction, d->SelectAllAction, d->CopyAction })
  {
    Action->setShortcutContext(Qt::WidgetShortcut);
    d->FileView->addAction(Action);
  }

  connect(d->FocusAction, &QAction::triggered, d, [this]() { d->FocusFile(); });
  connect(d->EditAction, &QAction::triggered, this, [this]() { emit EditRequested(d->SelectedPaths()); });
  connect(d->DownloadAction, &QAction::triggered, this, [this]() { emit DownloadRequested(d->SelectedPaths()); });
  connect(d->DeleteAction, &QAction::triggered, this, [this]() { emit DeleteRequested(d->SelectedPaths()); });
  connect(d->SelectAllAction, &QAction::triggered, d->FileView, &QTreeWidget::selectAll);
  connect(d->CopyAction, &QAction::triggered, d, [this]() { d->CopyToClipboard(); });
  connect(d->FocusButton, &QPushButton::clicked, d->FocusAction, &QAction::trigger);
  connect(d->EditButton, &QPushButton::clicked, d->EditAction, &QAction::trigger);
  connect(d->DownloadButton, &QPushButton::clicked, d->DownloadAction, &QAction::trigger);
  connect(d->DeleteButton, &QPushButton::clicked, d->DeleteAction, &QAction::trigger);
  connect(d->CopyButton, &QPushButton::clicked, d->CopyAction, &QAction::trigger);
  connect(d->StartStopButton, &QPushButton::clicked, d, [this]()
  {
    // StartStopButtonClick
    if (d->IsFinding())
    {
      d->Stop();
    }
    else
    {
      if (d->FilterGroup->isAncestorOf(focusWidget()))
      {
        d->FileView->setFocus();
      }
      d->Start();
    }
  });
  connect(d->MaskButton, &QPushButton::clicked, d, [this]()
  {
    QString Mask = d->MaskEdit->currentText();
    if (EditMaskDialog::EditMask(this, Mask))
    {
      d->MaskEdit->setEditText(Mask);
    }
  });
  connect(d->DirectoryEdit, &QComboBox::editTextChanged, d, [this]() { d->UpdateControls(); });
  connect(d->FileView, &QTreeWidget::itemSelectionChanged, d, [this]() { d->UpdateControls(); });
  connect(d->FileView, &QTreeWidget::currentItemChanged, d, [this]() { d->UpdateControls(); });
  connect(d->FileView, &QTreeWidget::itemDoubleClicked, d, [this]() { d->FocusFile(); });
  connect(d->FileView, &QTreeWidget::itemActivated, d, [this]() { d->FocusFile(); });
  connect(d->FileView->header(), &QHeaderView::sectionClicked, d, [this](int Column)
  {
    QHeaderView * Header = d->FileView->header();
    Qt::SortOrder Order = Qt::AscendingOrder;
    if (d->Sorted && (Header->sortIndicatorSection() == Column))
    {
      Order = (Header->sortIndicatorOrder() == Qt::AscendingOrder) ? Qt::DescendingOrder : Qt::AscendingOrder;
    }
    d->Sorted = true;
    Header->setSortIndicatorShown(true);
    Header->setSortIndicator(Column, Order);
    d->FileView->sortItems(Column, Order);
  });
  connect(d->FileView, &QWidget::customContextMenuRequested, d, [this](const QPoint & Pos)
  {
    d->UpdateControls();
    QMenu Menu(this);
    Menu.addAction(d->FocusAction);
    Menu.addSeparator();
    if (!d->EditButton->isHidden()) Menu.addAction(d->EditAction);
    if (!d->DownloadButton->isHidden()) Menu.addAction(d->DownloadAction);
    if (!d->DeleteButton->isHidden()) Menu.addAction(d->DeleteAction);
    Menu.addSeparator();
    Menu.addAction(d->SelectAllAction);
    Menu.addSeparator();
    Menu.addAction(d->CopyAction);
    Menu.exec(d->FileView->viewport()->mapToGlobal(Pos));
  });
  // Focus button is default while the list has focus
  connect(qApp, &QApplication::focusChanged, d, [this]() { d->UpdateControls(); });

  resize(640, 480);
  d->LoadState();
  Init(Term, StartDirectory);
}

FindFilesDialog::~FindFilesDialog()
{
  delete d;
}

void FindFilesDialog::Init(Terminal * Term, const QString & StartDirectory)
{
  bool Local = (Term == nullptr);
  if ((d->Term.data() != Term) || (d->Local != Local) || d->TerminalName.isEmpty())
  {
    if (d->IsFinding())
    {
      d->Stop();
    }
    d->Term = Term;
    d->Local = Local;
    d->TerminalName = Local ? QStringLiteral("Local") : Term->Data().SessionName();
    d->Clear();
    d->FState = Private::ffInit;
    d->MaskEdit->setFocus();
  }
  {
    QSignalBlocker MaskBlocker(d->MaskEdit);
    d->MaskEdit->clear();
    d->MaskEdit->addItems(HistoryComboBox::History(QStringLiteral("Mask")));
    d->MaskEdit->setEditText(EditMaskDialog::SelectMask());
  }
  {
    QSignalBlocker DirectoryBlocker(d->DirectoryEdit);
    d->DirectoryEdit->clear();
    d->DirectoryEdit->addItems(HistoryComboBox::History(d->DirectoryHistoryKey()));
  }
  QString Directory = StartDirectory;
  while ((Directory.size() > 1) && Directory.endsWith(QLatin1Char('/')))
  {
    Directory.chop(1);
  }
  d->DirectoryEdit->setEditText(Directory);
  d->UpdateControls();
}

void FindFilesDialog::FilesDeleted(const QStringList & Paths)
{
  for (int Index = d->FileView->topLevelItemCount() - 1; Index >= 0; Index--)
  {
    FoundItem * Item = static_cast<FoundItem *>(d->FileView->topLevelItem(Index));
    if (Paths.contains(Item->File.FullPath))
    {
      delete d->FileView->takeTopLevelItem(Index);
    }
  }
  d->UpdateControls();
}

void FindFilesDialog::showEvent(QShowEvent * Event)
{
  // Operations nobody handles are not offered
  d->EditButton->setVisible(isSignalConnected(QMetaMethod::fromSignal(&FindFilesDialog::EditRequested)));
  d->DownloadButton->setVisible(!d->Local && isSignalConnected(QMetaMethod::fromSignal(&FindFilesDialog::DownloadRequested)));
  d->DeleteButton->setVisible(isSignalConnected(QMetaMethod::fromSignal(&FindFilesDialog::DeleteRequested)));
  d->FocusButton->setVisible(isSignalConnected(QMetaMethod::fromSignal(&FindFilesDialog::FocusRequested)));
  d->EditAction->setEnabled(!d->EditButton->isHidden());
  d->UpdateControls();
  QDialog::showEvent(Event);
}

void FindFilesDialog::closeEvent(QCloseEvent * Event)
{
  // FormCloseQuery
  if (d->StopIfFinding())
  {
    d->ClosePending = true;
    Event->ignore();
    return;
  }
  d->SaveState();
  QDialog::closeEvent(Event);
}

void FindFilesDialog::reject()
{
  // Esc: stop the search, or close (CMDialogKey)
  if (!d->StopIfFinding())
  {
    close();
  }
}

void FindFilesDialog::keyPressEvent(QKeyEvent * Event)
{
  if ((Event->key() == Qt::Key_F10) && (Event->modifiers() == Qt::NoModifier))
  {
    d->StopIfFinding();
    d->ClosePending = d->IsFinding();
    if (!d->ClosePending)
    {
      close();
    }
    return;
  }
  if (((Event->key() == Qt::Key_Return) || (Event->key() == Qt::Key_Enter)) && d->FileView->hasFocus())
  {
    d->FocusFile();
    return;
  }
  QDialog::keyPressEvent(Event);
}
