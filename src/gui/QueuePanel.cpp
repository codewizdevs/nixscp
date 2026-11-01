// Port of WinSCP queue view: forms/CustomScpExplorer.dfm/.cpp (QueuePanel with
// QueueLabel, QueueToolbar, QueueView3 and QueueFileList; UpdateQueueView,
// ToggleQueueVisibility, GoToQueue, UpdateQueueLabel, UpdateQueueFileList,
// QueueResetLayoutColumns, IsQueueAutoPopup/UserActionTimer, the Once Empty
// handling of UpdateQueueStatus), forms/NonVisual.dfm/.cpp (Queue* actions,
// QueuePopup, QueueItemSpeedAction, Set/CycleQueueOnceEmptyAction) and
// windows/QueueController.cpp (TQueueController::FillQueueViewItem,
// UseDetailsLine, DefaultOperation, AllowOperation, ExecuteOperation).
#include "gui/QueuePanel.h"
#include "gui/Translator.h"

#include "core/Configuration.h"
#include "core/IniStorage.h"
#include "core/Queue.h"
#include "gui/FileListModel.h"
#include "gui/Icons.h"
#include "gui/PreferencesDialog.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QProcess>
#include <QScreen>
#include <QSplitter>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <algorithm>
#include <deque>

namespace
{

enum Column { ColOperation, ColSource, ColDestination, ColTransferred, ColTime, ColSpeed, ColProgress, ColCount };

// Item data roles
const int IdRole = Qt::UserRole;            // queue item id (on primary and details line)
const int PercentRole = Qt::UserRole + 1;   // progress bar value in Progress column, -1 = text only

// WinConfiguration defaults
const int DefaultQueueHeight = 140;         // QueueView.Height
const int DefaultFileListHeight = 90;       // QueueView.FileListHeight
const int MaxSpeedHistory = 10;

QString QueueViewLayoutDefault()
{
  // with 1000 pixels wide screen, both interfaces are wide enough to fit wider queue
  QScreen * Screen = QGuiApplication::primaryScreen();
  bool Wide = (Screen != nullptr) && (Screen->availableGeometry().width() > 1000);
  return QString(Wide ? QStringLiteral("70,250,250,80,80,80,100") : QStringLiteral("70,160,160,80,80,80,100")) +
    QStringLiteral(",;96");
}

// Configuration\Interface\QueueView (TQueueViewConfiguration); members that
// Configuration does not carry are read and written here directly
bool OpenQueueViewKey(IniStorage & Storage, bool CanCreate)
{
  return Storage.OpenSubKey(QStringLiteral("Configuration"), CanCreate) &&
    Storage.OpenSubKey(QStringLiteral("Interface"), CanCreate) &&
    Storage.OpenSubKey(QStringLiteral("QueueView"), CanCreate);
}

int ReadQueueViewInt(const QString & Name, int Default)
{
  IniStorage Storage(&Configuration::Instance().Ini());
  return OpenQueueViewKey(Storage, false) ? Storage.ReadInteger(Name, Default) : Default;
}

bool ReadQueueViewBool(const QString & Name, bool Default)
{
  IniStorage Storage(&Configuration::Instance().Ini());
  return OpenQueueViewKey(Storage, false) ? Storage.ReadBool(Name, Default) : Default;
}

QString ReadQueueViewString(const QString & Name, const QString & Default)
{
  IniStorage Storage(&Configuration::Instance().Ini());
  return OpenQueueViewKey(Storage, false) ? Storage.ReadString(Name, Default) : Default;
}

void WriteQueueViewInt(const QString & Name, int Value)
{
  IniStorage Storage(&Configuration::Instance().Ini());
  if (OpenQueueViewKey(Storage, true))
  {
    Storage.WriteInteger(Name, Value);
  }
}

void WriteQueueViewBool(const QString & Name, bool Value)
{
  IniStorage Storage(&Configuration::Instance().Ini());
  if (OpenQueueViewKey(Storage, true))
  {
    Storage.WriteBool(Name, Value);
  }
}

void WriteQueueViewString(const QString & Name, const QString & Value)
{
  IniStorage Storage(&Configuration::Instance().Ini());
  if (OpenQueueViewKey(Storage, true))
  {
    Storage.WriteString(Name, Value);
  }
}

// CustomWinConfiguration->History["SpeedLimit"] (Configuration\History\SpeedLimit)
QStringList SpeedHistory()
{
  QStringList Result;
  IniStorage Storage(&Configuration::Instance().Ini());
  if (Storage.OpenSubKey(QStringLiteral("Configuration"), false) &&
      Storage.OpenSubKey(QStringLiteral("History"), false) &&
      Storage.OpenSubKey(QStringLiteral("SpeedLimit"), false))
  {
    for (int Index = 0; ; Index++)
    {
      QString Name = QString::number(Index);
      if (!Storage.ValueExists(Name))
      {
        break;
      }
      Result.append(Storage.ReadString(Name, QString()));
    }
  }
  if (Result.isEmpty())
  {
    // Defaults for speed limits (TCustomWinConfiguration::DefaultHistory)
    Result.append(SetSpeedLimit(0));
    for (unsigned long Speed = MaxSpeed; Speed >= MinSpeed; Speed /= 2)
    {
      Result.append(SetSpeedLimit(Speed));
    }
  }
  // CopySpeedLimits: "Unlimited" first, invalid entries dropped
  QStringList Speeds;
  bool Unlimited = false;
  for (const QString & Text : Result)
  {
    unsigned long Speed;
    bool Valid = TryGetSpeedLimit(Text, Speed);
    if ((!Valid || (Speed == 0)) && !Unlimited)
    {
      Speeds.append(SetSpeedLimit(0));
      Unlimited = true;
    }
    else if (Valid && (Speed > 0))
    {
      Speeds.append(Text);
    }
  }
  if (!Unlimited)
  {
    Speeds.prepend(SetSpeedLimit(0));
  }
  return Speeds;
}

void SaveSpeedHistory(const QString & Value)
{
  // SaveToHistory: most recent first, no duplicates
  QStringList List = SpeedHistory();
  List.removeAll(Value);
  List.prepend(Value);
  while (List.size() > MaxSpeedHistory)
  {
    List.removeLast();
  }
  IniStorage Storage(&Configuration::Instance().Ini());
  if (Storage.OpenSubKey(QStringLiteral("Configuration"), true) &&
      Storage.OpenSubKey(QStringLiteral("History"), true) &&
      Storage.OpenSubKey(QStringLiteral("SpeedLimit"), true))
  {
    for (const QString & Name : Storage.GetValueNames())
    {
      Storage.DeleteValue(Name);
    }
    for (int Index = 0; Index < List.size(); Index++)
    {
      Storage.WriteString(QString::number(Index), List[Index]);
    }
  }
}

// TQueueController::QueueViewCustomDrawItem draws progress bars in the
// Progress column; here a delegate does
class ProgressDelegate : public QStyledItemDelegate
{
public:
  using QStyledItemDelegate::QStyledItemDelegate;

  void paint(QPainter * Painter, const QStyleOptionViewItem & Option, const QModelIndex & Index) const override
  {
    QVariant Percent = Index.data(PercentRole);
    if ((Index.column() != ColProgress) || !Percent.isValid() || (Percent.toInt() < 0))
    {
      QStyledItemDelegate::paint(Painter, Option, Index);
      return;
    }
    // background incl. selection
    QStyleOptionViewItem ItemOption = Option;
    initStyleOption(&ItemOption, Index);
    ItemOption.text.clear();
    QStyle * Style = ItemOption.widget ? ItemOption.widget->style() : QApplication::style();
    Style->drawControl(QStyle::CE_ItemViewItem, &ItemOption, Painter, ItemOption.widget);

    QStyleOptionProgressBar Bar;
    Bar.rect = Option.rect.adjusted(2, 2, -2, -2);
    Bar.minimum = 0;
    Bar.maximum = 100;
    Bar.progress = qBound(0, Percent.toInt(), 100);
    Bar.text = Index.data(Qt::DisplayRole).toString();
    Bar.textVisible = true;
    Bar.textAlignment = Qt::AlignCenter;
    Bar.state = Option.state | QStyle::State_Horizontal;
    Bar.palette = Option.palette;
    Bar.fontMetrics = Option.fontMetrics;
    Style->drawControl(QStyle::CE_ProgressBar, &Bar, Painter, ItemOption.widget);
  }
};

// QueueItemSpeedAction (TTBEditAction shown as TTBXComboBoxItem "&Speed")
class SpeedAction : public QWidgetAction
{
public:
  std::function<QString()> CurrentText;
  std::function<void(const QString &)> Accepted;

  explicit SpeedAction(QObject * Parent) : QWidgetAction(Parent) {}

  void Refresh()
  {
    QString Text = CurrentText ? CurrentText() : QString();
    for (QWidget * W : createdWidgets())
    {
      QComboBox * Combo = W->findChild<QComboBox *>();
      // only while its menu is closed (refreshed again when it opens)
      if ((Combo != nullptr) && !W->isVisible())
      {
        QSignalBlocker Blocker(Combo);
        Combo->clear();
        Combo->addItems(SpeedHistory());
        Combo->setEditText(Text);
      }
      W->setEnabled(isEnabled());
    }
  }

protected:
  QWidget * createWidget(QWidget * Parent) override
  {
    auto * Container = new QWidget(Parent);
    auto * Layout = new QHBoxLayout(Container);
    Layout->setContentsMargins(6, 2, 6, 2);
    auto * Label = new QLabel(QStringLiteral("&Speed"), Container);
    auto * Combo = new QComboBox(Container);
    Combo->setEditable(true);
    Combo->setInsertPolicy(QComboBox::NoInsert);
    Combo->setMinimumContentsLength(10);
    Combo->setToolTip(toolTip());
    Label->setBuddy(Combo);
    Layout->addWidget(Label);
    Layout->addWidget(Combo, 1);
    auto Accept = [this, Combo]()
    {
      if (Accepted)
      {
        Accepted(Combo->currentText());
      }
      // close the menu the combo box is in
      for (QWidget * W = Combo->parentWidget(); W != nullptr; W = W->parentWidget())
      {
        if (QMenu * Menu = qobject_cast<QMenu *>(W))
        {
          Menu->close();
        }
      }
    };
    QObject::connect(Combo, QOverload<int>::of(&QComboBox::activated), Container, [Accept](int) { Accept(); });
    QObject::connect(Combo->lineEdit(), &QLineEdit::returnPressed, Container, Accept);
    Combo->addItems(SpeedHistory());
    Combo->setEditText(CurrentText ? CurrentText() : QString());
    Container->setEnabled(isEnabled());
    return Container;
  }
};

}

// A QObject child of the panel, so it is freed with it (the header declares no
// destructor); connections use it as context, so none outlives it
struct QueuePanel::Private : public QObject
{
  explicit Private(QueuePanel * APanel) : QObject(APanel), Panel(APanel) {}

  QueuePanel * Panel = nullptr;
  QPointer<TransferQueue> Queue;
  QLabel * Label = nullptr;
  QToolBar * Toolbar = nullptr;
  QSplitter * Splitter = nullptr;
  QTreeWidget * View = nullptr;
  QTreeWidget * FileListView = nullptr;
  QMenu * Popup = nullptr;
  QMenu * OnceEmptyMenu = nullptr;
  QTimer RefreshTimer;
  QTimer LayoutSaveTimer;
  QList<QueueItemInfo> Items;   // as displayed
  QAction * Actions[static_cast<int>(QueueAction::Count)] = {};
  SpeedAction * Speed = nullptr;
  OnceEmptyOperation OnceEmpty = odoIdle;
  int PendingUserActionItem = 0;   // FPendingQueueActionItem
  bool ProcessingUserAction = false;
  QPointer<QWidget> ShortcutWindow;
  int FileListItem = -1;            // item shown in the file list
  QList<QPair<QString, QueueFileState>> FileList;
  bool LoadingLayout = false;

  // samples for speed (TFileOperationProgressType::CPS uses recent history)
  struct Sample
  {
    qint64 ElapsedMs;
    qint64 Transferred;
  };
  QHash<int, std::deque<Sample>> Samples;

  QAction * A(QueueAction Id) const { return Actions[static_cast<int>(Id)]; }
  QAction * MakeAction(QueueAction Id, const QString & Caption, const QString & Hint, const QString & ActionName);
  void Refresh();
  void FillRow(QTreeWidgetItem * Row, const QueueItemInfo & Info, bool Detail, bool OnlyLine, bool BlinkHide,
    const FileOperationProgress::Snapshot * DetailProgress = nullptr);
  QList<FileOperationProgress::Snapshot> DetailLines(const QueueItemInfo & Info) const;
  bool UseDetailsLine(const QueueItemInfo & Info) const;
  qint64 CPS(const QueueItemInfo & Info);
  int SelectedId() const;
  const QueueItemInfo * SelectedItem() const;
  int IndexOf(int Id) const;
  static bool IsPending(const QueueItemInfo & Info);
  void UpdateActions();
  void DefaultOperation();
  void ProcessUserAction(int Id);
  void CheckAutoPopup();
  void UpdateLabel();
  void UpdateLabelStatus();
  void UpdateFileList();
  void LoadLayout(const QString & Layout);
  QString SaveLayout() const;
  void SetQueueViewShow(TQueueViewShow Show);
  void ApplyVisibility();
  void LoadViewOptions();
  void HandleEmptied();
  void InstallWindowShortcuts();
  static bool Busy();
};

QAction * QueuePanel::Private::MakeAction(QueueAction Id, const QString & Caption, const QString & Hint, const QString & ActionName)
{
  QAction * Action = new QAction(Caption, Panel);
  if (!ActionName.isEmpty())
  {
    Action->setIcon(Icons::ForAction(ActionName));
  }
  Action->setToolTip(Hint);
  Action->setStatusTip(Hint);
  Action->setObjectName(ActionName);
  Actions[static_cast<int>(Id)] = Action;
  return Action;
}

static QString FormatTimeSpan(qint64 Seconds)
{
  // FormatDateTimeSpan (core/Common.cpp)
  if (Seconds < 0)
  {
    Seconds = 0;
  }
  qint64 Days = Seconds / 86400;
  if (Days >= 4)
  {
    return QStringLiteral("%1 days").arg(Days);
  }
  return QStringLiteral("%1:%2:%3")
    .arg(Seconds / 3600)
    .arg((Seconds / 60) % 60, 2, 10, QLatin1Char('0'))
    .arg(Seconds % 60, 2, 10, QLatin1Char('0'));
}

static int OverallProgress(const FileOperationProgress::Snapshot & P)
{
  // TFileOperationProgressType::OverallProgress
  if (P.TotalSize > 0)
  {
    return static_cast<int>(qBound<qint64>(0LL, P.TotalTransferred * 100 / P.TotalSize, 100LL));
  }
  if (P.Count > 0)
  {
    return qBound(0, P.FilesFinished * 100 / P.Count, 100);
  }
  return 0;
}

static int TransferProgress(const FileOperationProgress::Snapshot & P)
{
  // TFileOperationProgressType::TransferProgress
  if (P.FileSize > 0)
  {
    return static_cast<int>(qBound<qint64>(0LL, P.FileTransferred * 100 / P.FileSize, 100LL));
  }
  return 0;
}

// TTransferQueueItem source info: single file as is, more files as their common directory
static QString SourceStr(const QueueItemInfo & Info)
{
  if (Info.Sources.isEmpty())
  {
    return QString();
  }
  if (Info.Sources.size() == 1)
  {
    return Info.Sources.first();
  }
  // ExtractLocalSourcePath / ExtractRemoteSourcePath: common path of the files
  QString Common = Info.Sources.first();
  int Slash = Common.lastIndexOf(QLatin1Char('/'));
  Common = (Slash > 0) ? Common.left(Slash) : QStringLiteral("/");
  for (const QString & Source : Info.Sources)
  {
    while ((Common != QStringLiteral("/")) && !Source.startsWith(Common + QLatin1Char('/')))
    {
      int Pos = Common.lastIndexOf(QLatin1Char('/'));
      Common = (Pos > 0) ? Common.left(Pos) : QStringLiteral("/");
    }
  }
  return Common;
}

bool QueuePanel::Private::IsPending(const QueueItemInfo & Info)
{
  // not started yet: pending, or suspended before it started
  return (Info.State == QueueItemInfo::qsPending) ||
    ((Info.State == QueueItemInfo::qsPaused) && !Info.Progress.InProgress && (Info.Progress.ElapsedMs == 0));
}

bool QueuePanel::Private::UseDetailsLine(const QueueItemInfo & Info) const
{
  // TQueueController::UseDetailsLine: active transfer of more than one file
  if (Info.Type != QueueItemInfo::ikTransfer)
  {
    return false;
  }
  bool Active = (Info.State == QueueItemInfo::qsProcessing) ||
    ((Info.State == QueueItemInfo::qsPaused) && !IsPending(Info)) ||
    (QueueItemInfo::IsUserActionStatus(Info.State) && !Info.Finished && Info.Progress.InProgress);
  if (!Active || Info.Listing)
  {
    return false;
  }
  bool SingleFile = (Info.Sources.size() == 1) && (Info.Progress.Count <= 1) &&
    (Info.Progress.FileName.isEmpty() || (Info.Progress.FileName == Info.Sources.first()));
  return !SingleFile;
}

qint64 QueuePanel::Private::CPS(const QueueItemInfo & Info)
{
  std::deque<Sample> & S = Samples[Info.Id];
  const FileOperationProgress::Snapshot & P = Info.Progress;
  if (!S.empty() && ((P.ElapsedMs < S.back().ElapsedMs) || (P.TotalTransferred < S.back().Transferred)))
  {
    S.clear(); // restarted (connection reused)
  }
  if (S.empty() || (S.back().ElapsedMs != P.ElapsedMs))
  {
    S.push_back(Sample{P.ElapsedMs, P.TotalTransferred});
  }
  while ((S.size() > 2) && (P.ElapsedMs - S.front().ElapsedMs > 5000))
  {
    S.pop_front();
  }
  qint64 Ms = P.ElapsedMs - S.front().ElapsedMs;
  qint64 Bytes = P.TotalTransferred - S.front().Transferred;
  if (Ms < 500)
  {
    // not enough history yet: overall average
    return (P.ElapsedMs > 0) ? (P.TotalTransferred * 1000 / P.ElapsedMs) : 0;
  }
  return Bytes * 1000 / Ms;
}

// TQueueController::FillQueueViewItem
// Details lines: the file of each connection of a parallel transfer (the
// non-primary TParallelTransferQueueItem lines), else the file being
// transferred when the item has more files (UseDetailsLine)
QList<FileOperationProgress::Snapshot> QueuePanel::Private::DetailLines(const QueueItemInfo & Info) const
{
  QList<FileOperationProgress::Snapshot> Result;
  if (Info.ConnectionProgress.size() > 1)
  {
    for (const FileOperationProgress::Snapshot & P : Info.ConnectionProgress)
    {
      if (!P.FileName.isEmpty())
      {
        Result.append(P);
      }
    }
  }
  if (Result.isEmpty() && UseDetailsLine(Info))
  {
    Result.append(Info.Progress);
  }
  return Result;
}

void QueuePanel::Private::FillRow(QTreeWidgetItem * Row, const QueueItemInfo & Info, bool Detail, bool OnlyLine, bool BlinkHide,
  const FileOperationProgress::Snapshot * DetailProgress)
{
  QString ProgressStr;
  QIcon Image;
  switch (Info.State)
  {
    case QueueItemInfo::qsDone:
      ProgressStr = Translator::Tr(QStringLiteral("Completed"));
      break;
    case QueueItemInfo::qsPending:
      ProgressStr = Translator::Tr(QStringLiteral("Waiting..."));
      break;
    case QueueItemInfo::qsConnecting:
      ProgressStr = Translator::Tr(QStringLiteral("Connecting..."));
      break;
    case QueueItemInfo::qsQuery:
      ProgressStr = Translator::Tr(QStringLiteral("Query"));
      Image = Icons::Get(QStringLiteral("queue-item-query"));
      break;
    case QueueItemInfo::qsError:
      ProgressStr = Translator::Tr(QStringLiteral("Error"));
      Image = Icons::Get(QStringLiteral("queue-item-error"));
      break;
    case QueueItemInfo::qsPrompt:
      ProgressStr = Translator::Tr(QStringLiteral("Prompt"));
      Image = Icons::Get(QStringLiteral("queue-item-prompt"));
      break;
    case QueueItemInfo::qsPaused:
      ProgressStr = Translator::Tr(QStringLiteral("Suspended"));
      Image = Icons::Get(QStringLiteral("queue-item-paused"));
      break;
    case QueueItemInfo::qsProcessing:
      break;
  }

  QString Values[ColCount];
  QString OperationHint;
  int Percent = -1;
  const FileOperationProgress::Snapshot & P = Info.Progress;
  bool Started = (Info.State == QueueItemInfo::qsProcessing) || (Info.State == QueueItemInfo::qsDone) ||
    ((Info.State == QueueItemInfo::qsPaused) && !IsPending(Info)) ||
    (QueueItemInfo::IsUserActionStatus(Info.State) && (P.ElapsedMs > 0));
  QIcon State;

  if (Info.Type == QueueItemInfo::ikDelete)
  {
    // SimpleOperation
    State = Icons::Get(QStringLiteral("queue-item-delete"));
    OperationHint = Translator::Tr(QStringLiteral("Delete"));
    if (Started && (Info.State != QueueItemInfo::qsDone) && !P.FileName.isEmpty())
    {
      Values[ColSource] = P.FileName;
      Values[ColTime] = FormatTimeSpan(P.ElapsedMs / 1000);
      if (ProgressStr.isEmpty())
      {
        if (P.Count > 1)
        {
          Percent = OverallProgress(P);
          ProgressStr = QStringLiteral("%1%").arg(Percent);
        }
        else
        {
          ProgressStr = Translator::Tr(QStringLiteral("Deleting..."));
        }
      }
    }
    else
    {
      Values[ColSource] = SourceStr(Info);
    }
    Values[ColProgress] = ProgressStr;
  }
  else if (!Detail)
  {
    if (Info.Move)
    {
      State = Icons::Get(Info.ToRemote ? QStringLiteral("queue-item-upload-move") : QStringLiteral("queue-item-download-move"));
      OperationHint = Info.ToRemote ? Translator::Tr(QStringLiteral("Upload and Delete")) : Translator::Tr(QStringLiteral("Download and Delete"));
    }
    else
    {
      State = Icons::Get(Info.ToRemote ? QStringLiteral("queue-item-upload") : QStringLiteral("queue-item-download"));
      OperationHint = Info.ToRemote ? Translator::Tr(QStringLiteral("Upload")) : Translator::Tr(QStringLiteral("Download"));
    }
    if (!OnlyLine)
    {
      // status goes to the details line
      Image = QIcon();
      ProgressStr.clear();
    }
    Values[ColSource] = SourceStr(Info);
    Values[ColDestination] = Info.TargetDirectory;
    if (Started)
    {
      Values[ColTransferred] = FileListModel::FormatPanelBytes(P.TotalTransferred, Configuration::Instance().FormatSizeBytes);
    }
    if (Info.Listing)
    {
      // ProgressData->Operation == foCalculateSize
      if (ProgressStr.isEmpty())
      {
        ProgressStr = Translator::Tr(QStringLiteral("Listing..."));
      }
    }
    else if (Started)
    {
      if ((Info.State != QueueItemInfo::qsDone) && !Info.Finished)
      {
        // GetTime: time left when total size is known, elapsed otherwise
        qint64 Speed = 0;
        if (Info.State == QueueItemInfo::qsPaused)
        {
          Samples.remove(Info.Id); // start over on resume
        }
        else
        {
          Speed = CPS(Info);
        }
        qint64 Seconds;
        if ((P.TotalSize > 0) && (Speed > 0))
        {
          Seconds = (P.TotalSize - P.TotalTransferred) / Speed;
        }
        else
        {
          Seconds = P.ElapsedMs / 1000;
        }
        Values[ColTime] = FormatTimeSpan(Seconds);
        Values[ColSpeed] = QStringLiteral("%1/s").arg(FileListModel::FormatBytes(Speed));
      }
      if (ProgressStr.isEmpty())
      {
        Percent = OverallProgress(P);
        ProgressStr = QStringLiteral("%1%").arg(Percent);
      }
    }
    Values[ColProgress] = ProgressStr;
  }
  else
  {
    // details line: file being transferred
    const FileOperationProgress::Snapshot & D = (DetailProgress != nullptr) ? *DetailProgress : P;
    Values[ColSource] = D.FileName;
    Values[ColTransferred] = FileListModel::FormatPanelBytes(D.FileTransferred, Configuration::Instance().FormatSizeBytes);
    if (ProgressStr.isEmpty())
    {
      Percent = TransferProgress(D);
      ProgressStr = QStringLiteral("%1%").arg(Percent);
    }
    Values[ColProgress] = ProgressStr;
  }

  for (int Col = 0; Col < ColCount; Col++)
  {
    if (Row->text(Col) != Values[Col])
    {
      Row->setText(Col, Values[Col]);
    }
  }
  Row->setToolTip(ColSource, Values[ColSource]);
  Row->setToolTip(ColDestination, Values[ColDestination]);
  Row->setData(0, IdRole, Info.Id);
  Row->setData(ColProgress, PercentRole, Percent);
  // the operation column shows the images only (StateIndex/ImageIndex);
  // user action items blink
  QIcon RowIcon = BlinkHide ? QIcon() : (!Image.isNull() ? Image : State);
  Row->setIcon(ColOperation, RowIcon);
  // done items are grayed (QueueViewCustomDrawItem)
  QBrush Foreground = (Info.State == QueueItemInfo::qsDone) ?
    View->palette().brush(QPalette::Disabled, QPalette::Text) : QBrush();
  for (int Col = 0; Col < ColCount; Col++)
  {
    if (Row->foreground(Col) != Foreground)
    {
      Row->setForeground(Col, Foreground);
    }
  }
  QString Hint = OperationHint;
  if (Info.State == QueueItemInfo::qsError)
  {
    Hint = Info.Finished ? Info.Error : Info.UserActionText;
  }
  else if (QueueItemInfo::IsUserActionStatus(Info.State))
  {
    Hint = Info.UserActionText;
  }
  if (!Detail)
  {
    Row->setToolTip(ColOperation, Hint);
    Row->setToolTip(ColProgress, (Hint != OperationHint) ? Hint : QString());
  }
}

int QueuePanel::Private::IndexOf(int Id) const
{
  for (int Index = 0; Index < Items.size(); Index++)
  {
    if (Items[Index].Id == Id)
    {
      return Index;
    }
  }
  return -1;
}

int QueuePanel::Private::SelectedId() const
{
  QTreeWidgetItem * Row = View->currentItem();
  if ((Row == nullptr) || !Row->isSelected())
  {
    return 0;
  }
  if (Row->parent() != nullptr)
  {
    Row = Row->parent();
  }
  return Row->data(0, IdRole).toInt();
}

const QueueItemInfo * QueuePanel::Private::SelectedItem() const
{
  int Index = IndexOf(SelectedId());
  return (Index >= 0) ? &Items[Index] : nullptr;
}

// TCustomScpExplorerForm::RefreshQueueItems + TQueueController::UpdateQueueStatus
void QueuePanel::Private::Refresh()
{
  int Selected = SelectedId();
  Items = Queue ? Queue->Items() : QList<QueueItemInfo>();

  // drop speed history of items that are gone
  for (auto It = Samples.begin(); It != Samples.end(); )
  {
    if (IndexOf(It.key()) < 0)
    {
      It = Samples.erase(It);
    }
    else
    {
      ++It;
    }
  }

  bool BlinkPhase = (QDateTime::currentMSecsSinceEpoch() % 1000) >= 500;
  View->setUpdatesEnabled(false);
  {
    QSignalBlocker Blocker(View);
    // reuse rows in order, so the view does not flicker and scroll position is kept
    for (int Index = 0; Index < Items.size(); Index++)
    {
      const QueueItemInfo & Info = Items[Index];
      QTreeWidgetItem * Row = View->topLevelItem(Index);
      if ((Row != nullptr) && (Row->data(0, IdRole).toInt() != Info.Id))
      {
        // remove rows of deleted items, if the item follows later
        int Later = -1;
        for (int Next = Index + 1; Next < View->topLevelItemCount(); Next++)
        {
          if (View->topLevelItem(Next)->data(0, IdRole).toInt() == Info.Id)
          {
            Later = Next;
            break;
          }
        }
        if (Later >= 0)
        {
          QTreeWidgetItem * Found = View->takeTopLevelItem(Later);
          View->insertTopLevelItem(Index, Found);
          Row = Found;
        }
        else
        {
          Row = nullptr;
        }
      }
      if (Row == nullptr)
      {
        Row = new QTreeWidgetItem();
        for (int Col = ColTransferred; Col <= ColSpeed; Col++)
        {
          Row->setTextAlignment(Col, Qt::AlignRight | Qt::AlignVCenter);
        }
        Row->setTextAlignment(ColProgress, Qt::AlignCenter);
        View->insertTopLevelItem(Index, Row);
      }
      QList<FileOperationProgress::Snapshot> Lines = DetailLines(Info);
      bool Details = !Lines.isEmpty();
      // QueueItemNeedsFrequentRefresh && !ProcessingUserAction
      bool Blink = QueueItemInfo::IsUserActionStatus(Info.State) && BlinkPhase &&
        !(ProcessingUserAction && (PendingUserActionItem == Info.Id));
      FillRow(Row, Info, false, !Details, Blink && !Details);
      while (Row->childCount() > Lines.size())
      {
        delete Row->takeChild(Row->childCount() - 1);
      }
      for (int Line = 0; Line < Lines.size(); Line++)
      {
        QTreeWidgetItem * DetailRow = Row->child(Line);
        if (DetailRow == nullptr)
        {
          DetailRow = new QTreeWidgetItem(Row);
          DetailRow->setTextAlignment(ColTransferred, Qt::AlignRight | Qt::AlignVCenter);
          DetailRow->setTextAlignment(ColProgress, Qt::AlignCenter);
          Row->setExpanded(true);
        }
        FillRow(DetailRow, Info, true, false, Blink, &Lines[Line]);
      }
    }
    while (View->topLevelItemCount() > Items.size())
    {
      delete View->takeTopLevelItem(View->topLevelItemCount() - 1);
    }

    // keep selection by item id
    View->clearSelection();
    QTreeWidgetItem * Current = nullptr;
    int Index = IndexOf(Selected);
    if (Index >= 0)
    {
      Current = View->topLevelItem(Index);
      View->setCurrentItem(Current);
      Current->setSelected(true);
    }
    else
    {
      View->setCurrentItem(nullptr);
    }
  }
  View->setUpdatesEnabled(true);
  InstallWindowShortcuts();
  UpdateActions();
  UpdateLabel();
  UpdateFileList();
  CheckAutoPopup();
}

// TQueueController::AllowOperation + TNonVisualDataModule::UpdateAction
void QueuePanel::Private::UpdateActions()
{
  bool HasQueue = !Queue.isNull();
  View->setEnabled(HasQueue);
  QAction * Enable = A(QueueAction::Enable);
  Enable->setEnabled(HasQueue);
  {
    QSignalBlocker Blocker(Enable);
    Enable->setChecked(HasQueue ? Queue->Enabled() : false);
  }
  const QueueItemInfo * Item = HasQueue ? SelectedItem() : nullptr;
  QueueItemInfo::Status State = (Item != nullptr) ? Item->State : QueueItemInfo::qsPending;
  bool Query = (Item != nullptr) && (State == QueueItemInfo::qsQuery);
  bool Error = (Item != nullptr) && (State == QueueItemInfo::qsError);
  bool Prompt = (Item != nullptr) && (State == QueueItemInfo::qsPrompt);
  bool Execute = (Item != nullptr) && (State == QueueItemInfo::qsPending);
  bool Pause = (Item != nullptr) && (State == QueueItemInfo::qsProcessing);
  bool Resume = (Item != nullptr) && (State == QueueItemInfo::qsPaused);
  auto SetEnabledVisible = [](QAction * Action, bool Enabled, bool Visible)
  {
    Action->setEnabled(Enabled);
    Action->setVisible(Visible);
  };
  SetEnabledVisible(A(QueueAction::ItemQuery), Query, Query);
  SetEnabledVisible(A(QueueAction::ItemError), Error, Error);
  SetEnabledVisible(A(QueueAction::ItemPrompt), Prompt, Prompt);
  SetEnabledVisible(A(QueueAction::ItemExecute), Execute, Execute || (!Pause && !Resume));
  SetEnabledVisible(A(QueueAction::ItemPause), Pause, Pause);
  SetEnabledVisible(A(QueueAction::ItemResume), Resume, Resume);
  A(QueueAction::ItemDelete)->setEnabled(Item != nullptr);
  int Index = (Item != nullptr) ? IndexOf(Item->Id) : -1;
  A(QueueAction::ItemUp)->setEnabled(Execute && (Index > 0) && (Items[Index - 1].State == QueueItemInfo::qsPending));
  A(QueueAction::ItemDown)->setEnabled(Execute && (Index >= 0) && (Index + 1 < Items.size()));
  Speed->setEnabled((Item != nullptr) && (State != QueueItemInfo::qsDone) && !Item->Finished &&
    (Item->Type == QueueItemInfo::ikTransfer));
  Speed->Refresh();
  bool AnyRunning = false;
  bool AnyPaused = false;
  bool AnyDone = false;
  for (const QueueItemInfo & Info : Items)
  {
    AnyRunning = AnyRunning || (Info.State == QueueItemInfo::qsProcessing);
    AnyPaused = AnyPaused || (Info.State == QueueItemInfo::qsPaused);
    AnyDone = AnyDone || (Info.State == QueueItemInfo::qsDone);
  }
  A(QueueAction::PauseAll)->setEnabled(HasQueue && AnyRunning);
  A(QueueAction::ResumeAll)->setEnabled(HasQueue && AnyPaused);
  A(QueueAction::DeleteAll)->setEnabled(HasQueue && !Items.isEmpty());
  A(QueueAction::DeleteAllDone)->setEnabled(HasQueue && AnyDone);
  // qoOnceEmpty: IsAnythingQueued
  bool AnythingQueued = HasQueue && (Queue->ActiveCount() > 0);
  for (QueueAction Id : {QueueAction::CycleOnceEmpty, QueueAction::IdleOnceEmpty, QueueAction::DisconnectOnceEmpty,
       QueueAction::SuspendOnceEmpty, QueueAction::ShutDownOnceEmpty})
  {
    A(Id)->setEnabled(AnythingQueued);
  }
  QueueAction Current = QueueAction::IdleOnceEmpty;
  switch (OnceEmpty)
  {
    case odoIdle: Current = QueueAction::IdleOnceEmpty; break;
    case odoDisconnect: Current = QueueAction::DisconnectOnceEmpty; break;
    case odoSuspend: Current = QueueAction::SuspendOnceEmpty; break;
    case odoShutDown: Current = QueueAction::ShutDownOnceEmpty; break;
  }
  A(Current)->setChecked(true);
  QAction * Cycle = A(QueueAction::CycleOnceEmpty);
  Cycle->setIcon(A(Current)->icon());
  {
    QSignalBlocker Blocker(Cycle);
    Cycle->setChecked(OnceEmpty != odoIdle);
  }
  A(QueueAction::GoTo)->setEnabled(Panel->isVisible() && View->isEnabled());
  TQueueViewShow Show = Configuration::Instance().QueueViewShow;
  A(QueueAction::Show)->setChecked(Show == qvShow);
  A(QueueAction::HideWhenEmpty)->setChecked(Show == qvHideWhenEmpty);
  A(QueueAction::Hide)->setChecked(Show == qvHide);
  {
    QSignalBlocker Blocker(A(QueueAction::ToggleShow));
    A(QueueAction::ToggleShow)->setChecked(Panel->isVisible());
  }
  {
    QSignalBlocker Blocker(A(QueueAction::Toolbar));
    A(QueueAction::Toolbar)->setChecked(Toolbar->isVisibleTo(Panel));
  }
  {
    QSignalBlocker Blocker(A(QueueAction::FileList));
    A(QueueAction::FileList)->setChecked(FileListView->isVisibleTo(Panel));
  }
}

// TQueueController::DefaultOperation
void QueuePanel::Private::DefaultOperation()
{
  const QueueItemInfo * Item = SelectedItem();
  if ((Item == nullptr) || !Queue)
  {
    return;
  }
  int Id = Item->Id;
  switch (Item->State)
  {
    case QueueItemInfo::qsPending:
      Queue->Resume(Id); // Execute Now
      break;
    case QueueItemInfo::qsQuery:
    case QueueItemInfo::qsError:
    case QueueItemInfo::qsPrompt:
      ProcessUserAction(Id);
      break;
    case QueueItemInfo::qsProcessing:
      Queue->Pause(Id);
      break;
    case QueueItemInfo::qsPaused:
      Queue->Resume(Id);
      break;
    default:
      break;
  }
}

void QueuePanel::Private::ProcessUserAction(int Id)
{
  if (!Queue || ProcessingUserAction)
  {
    return;
  }
  QPointer<QueuePanel> Guard(Panel);
  QPointer<TransferQueue> QueueGuard(Queue);
  ProcessingUserAction = true;
  PendingUserActionItem = Id;
  Queue->ProcessUserAction(Id);
  if (!Guard)
  {
    return;
  }
  ProcessingUserAction = false;
  PendingUserActionItem = 0;
  if (QueueGuard)
  {
    Refresh();
  }
}

bool QueuePanel::Private::Busy()
{
  // NonVisualDataModule->Busy: a modal dialog or menu is open
  return (QApplication::activeModalWidget() != nullptr) || (QApplication::activePopupWidget() != nullptr);
}

// TCustomScpExplorerForm::RefreshQueueItems/UserActionTimer: with
// QueueAutoPopup a pending query, prompt or error pops up once idle
void QueuePanel::Private::CheckAutoPopup()
{
  if (!Queue || ProcessingUserAction || (PendingUserActionItem != 0) || !Configuration::Instance().QueueAutoPopup || Busy())
  {
    return;
  }
  int Id = Queue->UserActionItem();
  if (Id == 0)
  {
    return;
  }
  PendingUserActionItem = Id;
  QTimer::singleShot(10, this, [this, Id]()
  {
    PendingUserActionItem = 0;
    if (Queue && Configuration::Instance().QueueAutoPopup && !Busy() && (Queue->UserActionItem() == Id))
    {
      ProcessUserAction(Id);
    }
  });
}

// TCustomScpExplorerForm::UpdateQueueLabel: "Queue (N)"
void QueuePanel::Private::UpdateLabel()
{
  QString Caption = QStringLiteral("Queue");
  int Count = Queue ? Queue->ActiveCount() : 0;
  if (Count > 0)
  {
    Caption = QStringLiteral("%1 (%2)").arg(Caption).arg(Count);
  }
  if (Label->text() != Caption)
  {
    Label->setText(Caption);
  }
}

// TPathLabel active/inactive colors (QueueLabelGetStatus: list focused)
void QueuePanel::Private::UpdateLabelStatus()
{
  QWidget * Focus = QApplication::focusWidget();
  bool Active = (Focus != nullptr) && ((Focus == View) || (Focus == FileListView));
  QPalette Palette = Panel->palette();
  if (Active)
  {
    Palette.setColor(QPalette::Window, Palette.color(QPalette::Active, QPalette::Highlight));
    Palette.setColor(QPalette::WindowText, Palette.color(QPalette::Active, QPalette::HighlightedText));
  }
  else
  {
    Palette.setColor(QPalette::Window, Palette.color(QPalette::Active, QPalette::Button).darker(110));
    Palette.setColor(QPalette::WindowText, Palette.color(QPalette::Active, QPalette::ButtonText));
  }
  Label->setPalette(Palette);
}

// TCustomScpExplorerForm::UpdateQueueFileList
void QueuePanel::Private::UpdateFileList()
{
  if (!FileListView->isVisibleTo(Panel))
  {
    if (FileListView->topLevelItemCount() > 0)
    {
      FileListView->clear();
      FileList.clear();
      FileListItem = -1;
    }
    return;
  }
  int Id = SelectedId();
  QList<QPair<QString, QueueFileState>> List = (Queue && (Id != 0)) ? Queue->ItemFileList(Id) : QList<QPair<QString, QueueFileState>>();
  if ((Id == FileListItem) && (List == FileList))
  {
    return;
  }
  bool Reset = (Id != FileListItem) || (List.size() != FileList.size());
  FileListItem = Id;
  FileList = List;
  FileListView->setUpdatesEnabled(false);
  if (Reset)
  {
    FileListView->clear();
  }
  QBrush Gray = FileListView->palette().brush(QPalette::Disabled, QPalette::Text);
  for (int Index = 0; Index < List.size(); Index++)
  {
    QTreeWidgetItem * Row = Reset ? new QTreeWidgetItem(FileListView) : FileListView->topLevelItem(Index);
    if (Row->text(0) != List[Index].first)
    {
      Row->setText(0, List[Index].first);
    }
    // QueueFileListCustomDrawItem: processed files are grayed
    Row->setForeground(0, (List[Index].second == qfsProcessed) ? Gray : QBrush());
  }
  FileListView->setUpdatesEnabled(true);
}

// LoadListViewStr: "w1,w2,...,;ppi"
void QueuePanel::Private::LoadLayout(const QString & ALayout)
{
  LoadingLayout = true;
  QString Layout = ALayout;
  int Semicolon = Layout.indexOf(QLatin1Char(';'));
  int PixelsPerInch = 96;
  if (Semicolon >= 0)
  {
    bool Ok;
    int Ppi = Layout.mid(Semicolon + 1).section(QLatin1Char(';'), 0, 0).toInt(&Ok);
    if (Ok && (Ppi > 0))
    {
      PixelsPerInch = Ppi;
    }
    Layout = Layout.left(Semicolon);
  }
  const QStringList Widths = Layout.split(QLatin1Char(','));
  for (int Col = 0; (Col < ColCount) && (Col < Widths.size()); Col++)
  {
    bool Ok;
    int Width = Widths[Col].trimmed().toInt(&Ok);
    if (Ok && (Width > 0))
    {
      // LoadDimension: Qt widths are logical pixels at 96 DPI
      View->setColumnWidth(Col, Width * 96 / PixelsPerInch);
    }
  }
  LoadingLayout = false;
}

// GetListViewStr
QString QueuePanel::Private::SaveLayout() const
{
  QStringList Widths;
  for (int Col = 0; Col < ColCount; Col++)
  {
    Widths.append(QString::number(View->columnWidth(Col)));
  }
  // the comma before ";" keeps old versions parsing the list (WORKAROUND in GetListViewStr)
  return Widths.join(QLatin1Char(',')) + QStringLiteral(",;96");
}

// NonVisual QUEUEACTION(SHOW)
void QueuePanel::Private::SetQueueViewShow(TQueueViewShow Show)
{
  Configuration & Config = Configuration::Instance();
  if (Config.QueueViewShow != qvShow)
  {
    WriteQueueViewInt(QStringLiteral("LastHideShow"), Config.QueueViewShow);
  }
  Config.QueueViewShow = Show;
  ApplyVisibility();
}

void QueuePanel::Private::ApplyVisibility()
{
  TQueueViewShow Show = Configuration::Instance().QueueViewShow;
  // the main window's own logic shows the queue when not empty or forced
  Panel->setProperty("ForceShow", Show == qvShow);
  if (Show == qvHide)
  {
    emit Panel->HideRequested();
  }
  else if (Queue)
  {
    Panel->setVisible(Panel->ShouldBeVisible());
  }
  emit Panel->VisibilityChanged();
  UpdateActions();
}

void QueuePanel::Private::LoadViewOptions()
{
  QString Layout = ReadQueueViewString(QStringLiteral("Layout"), QString());
  LoadLayout(QueueViewLayoutDefault());
  if (!Layout.isEmpty())
  {
    LoadLayout(Layout);
  }
  Toolbar->setVisible(ReadQueueViewBool(QStringLiteral("ToolBar"), true));
  Label->setVisible(ReadQueueViewBool(QStringLiteral("Label"), true));
  FileListView->setVisible(ReadQueueViewBool(QStringLiteral("FileList"), false));
  int FileListHeight = ReadQueueViewInt(QStringLiteral("FileListHeight"), DefaultFileListHeight);
  Splitter->setSizes({1000, std::max(20, FileListHeight)});
  Panel->setProperty("ForceShow", Configuration::Instance().QueueViewShow == qvShow);
}

// TCustomScpExplorerForm::UpdateQueueStatus: Once Empty action when the queue empties
void QueuePanel::Private::HandleEmptied()
{
  OnceEmptyOperation Operation = OnceEmpty;
  // ResetQueueOnceEmptyOperation
  OnceEmpty = odoIdle;
  UpdateActions();
  if ((Operation == odoIdle) || Busy())
  {
    return;
  }
  static const char * Signal = SIGNAL(OnceEmptyRequested(int));
  if (Panel->receivers(Signal) > 0)
  {
    emit Panel->OnceEmptyRequested(Operation);
    return;
  }
  // Nobody to close the session: suspend/power off through systemd-logind
  // (SuspendWindows / ShutDownWindows of WinSCP)
  if (Operation == odoSuspend)
  {
    QProcess::startDetached(QStringLiteral("systemctl"), {QStringLiteral("suspend")});
  }
  else if (Operation == odoShutDown)
  {
    QProcess::startDetached(QStringLiteral("systemctl"), {QStringLiteral("poweroff")});
  }
}

// Ctrl+Shift+Q and Ctrl+Q work in the whole window, also while the queue is hidden
void QueuePanel::Private::InstallWindowShortcuts()
{
  QWidget * Window = Panel->window();
  if ((Window == Panel) || (Window == ShortcutWindow))
  {
    return;
  }
  if (ShortcutWindow)
  {
    ShortcutWindow->removeAction(A(QueueAction::Enable));
    ShortcutWindow->removeAction(A(QueueAction::GoTo));
  }
  ShortcutWindow = Window;
  Window->addAction(A(QueueAction::Enable));
  Window->addAction(A(QueueAction::GoTo));
}

QueuePanel::QueuePanel(QWidget * Parent) :
  QWidget(Parent),
  d(new Private(this))
{
  using QA = QueueAction;
  // Actions of NonVisual.dfm (captions, hints, shortcuts, image indexes)
  QAction * Enable = d->MakeAction(QA::Enable, QStringLiteral("&Process Queue"),
    QStringLiteral("Enable queue processing (pending queue items will not start, when queue processing is disabled)"),
    QStringLiteral("QueueEnableAction"));
  Enable->setCheckable(true);
  Enable->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Q));
  Enable->setShortcutContext(Qt::WindowShortcut);
  QAction * GoTo = d->MakeAction(QA::GoTo, QStringLiteral("&Go To"), QStringLiteral("Go to transfer queue list"),
    QStringLiteral("QueueGoToAction"));
  GoTo->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Q));
  GoTo->setShortcutContext(Qt::WindowShortcut);
  d->MakeAction(QA::ItemQuery, QStringLiteral("&Show Query"), QStringLiteral("Show pending query of selected queue item"),
    QStringLiteral("QueueItemQueryAction"));
  d->MakeAction(QA::ItemError, QStringLiteral("&Show Error"), QStringLiteral("Show pending error message of selected queue item"),
    QStringLiteral("QueueItemErrorAction"));
  d->MakeAction(QA::ItemPrompt, QStringLiteral("&Show Prompt"), QStringLiteral("Show pending prompt of selected queue item"),
    QStringLiteral("QueueItemPromptAction"));
  d->MakeAction(QA::ItemExecute, QStringLiteral("&Execute Now"),
    QStringLiteral("Execute selected queue item immediately by granting it additional connection"),
    QStringLiteral("QueueItemExecuteAction"));
  d->MakeAction(QA::ItemPause, QStringLiteral("&Suspend"), QStringLiteral("Suspend selected queue item"),
    QStringLiteral("QueueItemPauseAction"));
  d->MakeAction(QA::ItemResume, QStringLiteral("&Resume"), QStringLiteral("Resume selected suspended queue item"),
    QStringLiteral("QueueItemResumeAction"));
  d->MakeAction(QA::ItemDelete, QStringLiteral("&Cancel"), QStringLiteral("Remove selected queue item"),
    QStringLiteral("QueueItemDeleteAction"));
  d->Speed = new SpeedAction(this);
  d->Speed->setText(QStringLiteral("&Speed"));
  d->Speed->setToolTip(QStringLiteral("Change speed limit of selected queue item"));
  d->Speed->setStatusTip(d->Speed->toolTip());
  d->Speed->setObjectName(QStringLiteral("QueueItemSpeedAction"));
  d->Actions[static_cast<int>(QA::ItemSpeed)] = d->Speed;
  d->MakeAction(QA::ItemUp, QStringLiteral("Move &Up"), QStringLiteral("Move selected queue item up to be processed earlier"),
    QStringLiteral("QueueItemUpAction"));
  d->MakeAction(QA::ItemDown, QStringLiteral("Move &Down"), QStringLiteral("Move selected queue item down to be processed later"),
    QStringLiteral("QueueItemDownAction"));
  d->MakeAction(QA::PauseAll, QStringLiteral("&Suspend All"), QStringLiteral("Suspend all running queue items"),
    QStringLiteral("QueuePauseAllAction"));
  d->MakeAction(QA::ResumeAll, QStringLiteral("&Resume All"), QStringLiteral("Resume all suspended queue items"),
    QStringLiteral("QueueResumeAllAction"));
  d->MakeAction(QA::DeleteAll, QStringLiteral("&Cancel All"), QStringLiteral("Remove all queue items"),
    QStringLiteral("QueueDeleteAllAction"));
  d->MakeAction(QA::DeleteAllDone, QStringLiteral("Delete All &Completed"), QStringLiteral("Remove all completed queue items"),
    QStringLiteral("QueueDeleteAllDoneAction"));
  QAction * Cycle = d->MakeAction(QA::CycleOnceEmpty, QStringLiteral("Once &Empty"),
    QStringLiteral("Toggle action to perform once the queue list is empty"), QStringLiteral("QueueCycleOnceEmptyAction"));
  Cycle->setCheckable(true);
  auto * OnceEmptyGroup = new QActionGroup(this);
  struct
  {
    QA Id;
    const char * Caption;
    const char * Hint;
    const char * Name;
  } const OnceEmptyActions[] =
  {
    {QA::IdleOnceEmpty, "&Stay Idle", "Stay idle once the queue is empty", "QueueIdleOnceEmptyAction"},
    {QA::DisconnectOnceEmpty, "&Disconnect Session", "Disconnect the session once the queue is empty", "QueueDisconnectOnceEmptyAction2"},
    {QA::SuspendOnceEmpty, "Slee&p Computer", "Put the computer into sleep mode once the queue is empty", "QueueSuspendOnceEmptyAction2"},
    {QA::ShutDownOnceEmpty, "&Shut Down Computer", "Shut down the computer once the queue is empty", "QueueShutDownOnceEmptyAction2"},
  };
  for (const auto & Def : OnceEmptyActions)
  {
    QAction * Action = d->MakeAction(Def.Id, QString::fromLatin1(Def.Caption), QString::fromLatin1(Def.Hint), QString::fromLatin1(Def.Name));
    Action->setCheckable(true);
    OnceEmptyGroup->addAction(Action);
  }
  d->A(QA::IdleOnceEmpty)->setChecked(true);
  QAction * ToggleShow = d->MakeAction(QA::ToggleShow, QStringLiteral("&Queue"), QStringLiteral("Show/hide queue list"),
    QStringLiteral("QueueToggleShowAction"));
  ToggleShow->setCheckable(true);
  auto * ShowGroup = new QActionGroup(this);
  d->MakeAction(QA::Show, QStringLiteral("&Show"), QStringLiteral("Show queue list"), QString());
  d->MakeAction(QA::HideWhenEmpty, QStringLiteral("Hide when &Empty"), QStringLiteral("Hide queue list when it is empty"), QString());
  d->MakeAction(QA::Hide, QStringLiteral("&Hide"), QStringLiteral("Hide queue list"), QString());
  for (QA Id : {QA::Show, QA::HideWhenEmpty, QA::Hide})
  {
    d->A(Id)->setCheckable(true);
    ShowGroup->addAction(d->A(Id));
  }
  d->MakeAction(QA::Toolbar, QStringLiteral("&Toolbar"), QStringLiteral("Hide/show queue list toolbar (on queue list panel)"), QString())
    ->setCheckable(true);
  d->MakeAction(QA::FileList, QStringLiteral("&File List"), QStringLiteral("Hide/show full queue file list"), QString())
    ->setCheckable(true);
  d->MakeAction(QA::ResetLayoutColumns, QStringLiteral("&Reset Columns Layout"), QStringLiteral("Reset to the default layout of list columns"),
    QString());
  d->MakeAction(QA::Preferences, QStringLiteral("&Customize..."), QStringLiteral("Customize queue list"),
    QStringLiteral("QueuePreferencesAction"));

  // QueueLabel
  d->Label = new QLabel(QStringLiteral("Queue"), this);
  d->Label->setObjectName(QStringLiteral("QueueLabel"));
  d->Label->setAutoFillBackground(true);
  d->Label->setContentsMargins(4, 3, 4, 3);
  d->Label->setFocusPolicy(Qt::NoFocus);

  // QueueToolbar (CustomScpExplorer.dfm QueueToolbar)
  d->Toolbar = new QToolBar(this);
  d->Toolbar->setIconSize(QSize(16, 16));
  d->Toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
  d->Toolbar->addAction(Enable);
  d->Toolbar->addSeparator();
  for (QA Id : {QA::ItemQuery, QA::ItemError, QA::ItemPrompt, QA::ItemExecute, QA::ItemPause, QA::ItemResume, QA::ItemDelete})
  {
    d->Toolbar->addAction(d->A(Id));
  }
  d->Toolbar->addSeparator();
  d->Toolbar->addAction(d->A(QA::ItemUp));
  d->Toolbar->addAction(d->A(QA::ItemDown));
  d->Toolbar->addSeparator();
  d->Toolbar->addAction(d->A(QA::DeleteAllDone));
  d->Toolbar->addSeparator();
  d->OnceEmptyMenu = new QMenu(this);
  for (QA Id : {QA::IdleOnceEmpty, QA::DisconnectOnceEmpty, QA::SuspendOnceEmpty, QA::ShutDownOnceEmpty})
  {
    d->OnceEmptyMenu->addAction(d->A(Id));
  }
  Cycle->setMenu(d->OnceEmptyMenu);
  d->Toolbar->addAction(Cycle);
  if (QToolButton * Button = qobject_cast<QToolButton *>(d->Toolbar->widgetForAction(Cycle)))
  {
    Button->setPopupMode(QToolButton::MenuButtonPopup);
  }
  d->Toolbar->addAction(d->A(QA::Preferences));

  // QueueView3
  d->View = new QTreeWidget();
  d->View->setObjectName(QStringLiteral("QueueView3"));
  d->View->setColumnCount(ColCount);
  d->View->setHeaderLabels({QStringLiteral("Operation"), QStringLiteral("Source"), QStringLiteral("Destination"),
    QStringLiteral("Transferred"), QStringLiteral("Time"), QStringLiteral("Speed"), QStringLiteral("Progress")});
  d->View->headerItem()->setTextAlignment(ColTransferred, Qt::AlignRight | Qt::AlignVCenter);
  d->View->headerItem()->setTextAlignment(ColTime, Qt::AlignRight | Qt::AlignVCenter);
  d->View->headerItem()->setTextAlignment(ColSpeed, Qt::AlignRight | Qt::AlignVCenter);
  d->View->headerItem()->setTextAlignment(ColProgress, Qt::AlignCenter);
  d->View->header()->setSectionsClickable(false);
  d->View->header()->setStretchLastSection(false);
  d->View->header()->setMinimumSectionSize(20);   // MinWidth = 20
  d->View->setRootIsDecorated(false);
  d->View->setIndentation(0);
  d->View->setItemsExpandable(false);
  d->View->setExpandsOnDoubleClick(false);
  d->View->setUniformRowHeights(true);
  d->View->setSelectionMode(QAbstractItemView::SingleSelection);
  d->View->setSelectionBehavior(QAbstractItemView::SelectRows);
  d->View->setTextElideMode(Qt::ElideMiddle);
  d->View->setAllColumnsShowFocus(true);
  d->View->setContextMenuPolicy(Qt::CustomContextMenu);
  d->View->setItemDelegate(new ProgressDelegate(d->View));
  d->View->installEventFilter(this);

  // QueueFileList
  d->FileListView = new QTreeWidget();
  d->FileListView->setObjectName(QStringLiteral("QueueFileList"));
  d->FileListView->setColumnCount(1);
  d->FileListView->setHeaderHidden(true);
  d->FileListView->setRootIsDecorated(false);
  d->FileListView->setIndentation(0);
  d->FileListView->setUniformRowHeights(true);
  d->FileListView->setSelectionMode(QAbstractItemView::NoSelection);
  d->FileListView->setFocusPolicy(Qt::ClickFocus);
  d->FileListView->setTextElideMode(Qt::ElideMiddle);
  d->FileListView->setToolTip(QString());

  d->Splitter = new QSplitter(Qt::Vertical, this);
  d->Splitter->addWidget(d->View);
  d->Splitter->addWidget(d->FileListView);
  d->Splitter->setStretchFactor(0, 1);
  d->Splitter->setChildrenCollapsible(false);
  d->Splitter->handle(1)->setToolTip(QStringLiteral("Drag to resize queue file list. Double click to hide queue file list."));
  d->Splitter->handle(1)->installEventFilter(this);

  QVBoxLayout * Layout = new QVBoxLayout(this);
  Layout->setContentsMargins(0, 0, 0, 0);
  Layout->setSpacing(0);
  Layout->addWidget(d->Label);
  Layout->addWidget(d->Toolbar);
  Layout->addWidget(d->Splitter, 1);

  // QueuePopup (NonVisual.dfm)
  d->Popup = new QMenu(this);
  d->Popup->addAction(d->A(QA::ItemQuery));
  d->Popup->addAction(d->A(QA::ItemError));
  d->Popup->addAction(d->A(QA::ItemPrompt));
  d->Popup->addSeparator();
  for (QA Id : {QA::ItemExecute, QA::ItemPause, QA::ItemResume, QA::ItemDelete, QA::ItemSpeed})
  {
    d->Popup->addAction(d->A(Id));
  }
  d->Popup->addSeparator();
  d->Popup->addAction(d->A(QA::ItemUp));
  d->Popup->addAction(d->A(QA::ItemDown));
  d->Popup->addSeparator();
  d->Popup->addAction(Enable);
  QMenu * AllMenu = d->Popup->addMenu(QStringLiteral("&All"));
  AllMenu->addAction(d->A(QA::PauseAll));
  AllMenu->addAction(d->A(QA::ResumeAll));
  AllMenu->addAction(d->A(QA::DeleteAll));
  AllMenu->addSeparator();
  AllMenu->addAction(d->A(QA::DeleteAllDone));
  d->Popup->addAction(Cycle);
  QMenu * OptionsMenu = d->Popup->addMenu(QStringLiteral("&Options"));
  OptionsMenu->addAction(d->A(QA::Show));
  OptionsMenu->addAction(d->A(QA::HideWhenEmpty));
  OptionsMenu->addAction(d->A(QA::Hide));
  OptionsMenu->addSeparator();
  OptionsMenu->addAction(d->A(QA::Toolbar));
  OptionsMenu->addAction(d->A(QA::FileList));
  OptionsMenu->addSeparator();
  OptionsMenu->addAction(d->A(QA::ResetLayoutColumns));
  OptionsMenu->addSeparator();
  OptionsMenu->addAction(d->A(QA::Preferences));

  connect(d->View, &QWidget::customContextMenuRequested, d, [this](const QPoint & Pos)
  {
    d->UpdateActions();
    d->Popup->popup(d->View->viewport()->mapToGlobal(Pos));
  });
  connect(d->View, &QTreeWidget::itemSelectionChanged, d, [this]()
  {
    d->UpdateActions();
    d->UpdateFileList();
  });
  connect(d->View, &QTreeWidget::itemDoubleClicked, d, [this](QTreeWidgetItem *, int) { d->DefaultOperation(); });
  // details line is not selectable on its own: select its primary line
  connect(d->View, &QTreeWidget::currentItemChanged, d, [this](QTreeWidgetItem * Current, QTreeWidgetItem *)
  {
    if ((Current != nullptr) && (Current->parent() != nullptr))
    {
      d->View->setCurrentItem(Current->parent());
    }
  });
  connect(d->View->header(), &QHeaderView::sectionResized, d, [this]()
  {
    if (!d->LoadingLayout)
    {
      d->LayoutSaveTimer.start();
    }
  });
  d->LayoutSaveTimer.setSingleShot(true);
  d->LayoutSaveTimer.setInterval(300);
  connect(&d->LayoutSaveTimer, &QTimer::timeout, d, [this]()
  {
    WriteQueueViewString(QStringLiteral("Layout"), d->SaveLayout());
  });
  connect(d->Splitter, &QSplitter::splitterMoved, d, [this]()
  {
    if (d->FileListView->isVisible())
    {
      WriteQueueViewInt(QStringLiteral("FileListHeight"), d->FileListView->height());
      WriteQueueViewInt(QStringLiteral("FileListHeightPixelsPerInch"), 96);
    }
  });
  connect(qApp, &QApplication::focusChanged, d, [this]() { d->UpdateLabelStatus(); });

  auto WithSelected = [this](void (TransferQueue::*Method)(int))
  {
    return [this, Method]()
    {
      int Id = d->SelectedId();
      if (d->Queue && (Id != 0))
      {
        (d->Queue.data()->*Method)(Id);
      }
    };
  };
  connect(d->A(QA::ItemExecute), &QAction::triggered, d, WithSelected(&TransferQueue::Resume));
  connect(d->A(QA::ItemPause), &QAction::triggered, d, WithSelected(&TransferQueue::Pause));
  connect(d->A(QA::ItemResume), &QAction::triggered, d, WithSelected(&TransferQueue::Resume));
  connect(d->A(QA::ItemDelete), &QAction::triggered, d, WithSelected(&TransferQueue::Delete));
  connect(d->A(QA::ItemUp), &QAction::triggered, d, WithSelected(&TransferQueue::MoveUp));
  connect(d->A(QA::ItemDown), &QAction::triggered, d, WithSelected(&TransferQueue::MoveDown));
  for (QA Id : {QA::ItemQuery, QA::ItemError, QA::ItemPrompt})
  {
    connect(d->A(Id), &QAction::triggered, d, [this]()
    {
      int Id = d->SelectedId();
      if (Id != 0)
      {
        d->ProcessUserAction(Id);
      }
    });
  }
  connect(Enable, &QAction::triggered, d, [this](bool Checked)
  {
    // TCustomScpExplorerForm::ToggleQueueEnabled
    if (d->Queue)
    {
      d->Queue->SetEnabled(Checked);
    }
    d->UpdateActions();
  });
  connect(GoTo, &QAction::triggered, this, &QueuePanel::GoTo);
  connect(d->A(QA::PauseAll), &QAction::triggered, d, [this]() { if (d->Queue) d->Queue->PauseAll(); });
  connect(d->A(QA::ResumeAll), &QAction::triggered, d, [this]() { if (d->Queue) d->Queue->ResumeAll(); });
  connect(d->A(QA::DeleteAll), &QAction::triggered, d, [this]() { if (d->Queue) d->Queue->DeleteAll(); });
  connect(d->A(QA::DeleteAllDone), &QAction::triggered, d, [this]() { if (d->Queue) d->Queue->DeleteAllDone(); });
  d->Speed->CurrentText = [this]()
  {
    const QueueItemInfo * Item = d->SelectedItem();
    return (Item != nullptr) ? SetSpeedLimit(Item->CPSLimit) : QString();
  };
  d->Speed->Accepted = [this](const QString & Text)
  {
    // TNonVisualDataModule::QueueItemSpeed
    unsigned long Speed;
    int Id = d->SelectedId();
    if (!TryGetSpeedLimit(Text, Speed) || !d->Queue || (Id == 0))
    {
      return;
    }
    d->Queue->SetItemCPSLimit(Id, Speed);
    SaveSpeedHistory(SetSpeedLimit(Speed));
  };
  connect(Cycle, &QAction::triggered, d, [this]()
  {
    // TNonVisualDataModule::CycleQueueOnceEmptyAction
    switch (d->OnceEmpty)
    {
      case odoIdle: d->OnceEmpty = odoDisconnect; break;
      case odoDisconnect: d->OnceEmpty = odoSuspend; break;
      case odoSuspend: d->OnceEmpty = odoShutDown; break;
      case odoShutDown: d->OnceEmpty = odoIdle; break;
    }
    d->UpdateActions();
  });
  const QPair<QA, OnceEmptyOperation> OnceEmptyMap[] =
  {
    {QA::IdleOnceEmpty, odoIdle}, {QA::DisconnectOnceEmpty, odoDisconnect},
    {QA::SuspendOnceEmpty, odoSuspend}, {QA::ShutDownOnceEmpty, odoShutDown},
  };
  for (const auto & Pair : OnceEmptyMap)
  {
    OnceEmptyOperation Operation = Pair.second;
    connect(d->A(Pair.first), &QAction::triggered, d, [this, Operation]() { SetOnceEmpty(Operation); });
  }
  connect(ToggleShow, &QAction::triggered, this, &QueuePanel::ToggleVisibility);
  connect(d->A(QA::Show), &QAction::triggered, d, [this]() { d->SetQueueViewShow(qvShow); });
  connect(d->A(QA::HideWhenEmpty), &QAction::triggered, d, [this]() { d->SetQueueViewShow(qvHideWhenEmpty); });
  connect(d->A(QA::Hide), &QAction::triggered, d, [this]() { d->SetQueueViewShow(qvHide); });
  connect(d->A(QA::Toolbar), &QAction::triggered, d, [this](bool Checked)
  {
    d->Toolbar->setVisible(Checked);
    WriteQueueViewBool(QStringLiteral("ToolBar"), Checked);
  });
  connect(d->A(QA::FileList), &QAction::triggered, d, [this](bool Checked)
  {
    d->FileListView->setVisible(Checked);
    WriteQueueViewBool(QStringLiteral("FileList"), Checked);
    d->UpdateFileList();
  });
  connect(d->A(QA::ResetLayoutColumns), &QAction::triggered, d, [this]()
  {
    // TCustomScpExplorerForm::QueueResetLayoutColumns
    d->LoadLayout(QueueViewLayoutDefault());
    WriteQueueViewString(QStringLiteral("Layout"), d->SaveLayout());
  });
  connect(d->A(QA::Preferences), &QAction::triggered, d, [this]()
  {
    // PreferencesDialog(pmQueue)
    if (PreferencesDialog::Execute(window(), QStringLiteral("Background")))
    {
      d->ApplyVisibility();
      emit ConfigurationChanged();
    }
  });

  // progress of running items is polled while the panel is visible; user
  // action items blink and are popped up even when it is hidden
  d->RefreshTimer.setInterval(500);
  connect(&d->RefreshTimer, &QTimer::timeout, d, [this]()
  {
    if (!d->Queue)
    {
      return;
    }
    if (isVisible())
    {
      d->Refresh();
    }
    else
    {
      d->CheckAutoPopup();
    }
  });
  d->RefreshTimer.start();

  d->LoadViewOptions();
  d->UpdateLabelStatus();
  d->Refresh();
}

void QueuePanel::SetQueue(TransferQueue * Queue)
{
  if (d->Queue == Queue)
  {
    return;
  }
  if (d->Queue)
  {
    disconnect(d->Queue, nullptr, d, nullptr);
  }
  d->Queue = Queue;
  d->Samples.clear();
  d->FileListItem = -1;
  if (Queue != nullptr)
  {
    connect(Queue, &TransferQueue::Changed, d, [this]() { d->Refresh(); });
    connect(Queue, &TransferQueue::Emptied, d, [this]() { d->HandleEmptied(); });
    connect(Queue, &QObject::destroyed, d, [this]()
    {
      d->Queue = nullptr;
      d->Refresh();
    });
  }
  d->View->clear();
  d->Refresh();
}

TransferQueue * QueuePanel::Queue() const
{
  return d->Queue.data();
}

QAction * QueuePanel::Action(QueueAction Id) const
{
  return ((Id >= QueueAction::Enable) && (Id < QueueAction::Count)) ? d->A(Id) : nullptr;
}

void QueuePanel::PopulateQueueMenu(QMenu * Menu) const
{
  using QA = QueueAction;
  // ScpCommander.dfm QueueSubmenuItem
  QObject::connect(Menu, &QMenu::aboutToShow, d, [this]() { d->UpdateActions(); });
  Menu->addAction(d->A(QA::Enable));
  Menu->addAction(d->A(QA::GoTo));
  Menu->addSeparator();
  Menu->addAction(d->A(QA::ItemQuery));
  Menu->addAction(d->A(QA::ItemError));
  Menu->addAction(d->A(QA::ItemPrompt));
  Menu->addSeparator();
  for (QA Id : {QA::ItemExecute, QA::ItemPause, QA::ItemResume, QA::ItemDelete, QA::ItemSpeed})
  {
    Menu->addAction(d->A(Id));
  }
  Menu->addSeparator();
  Menu->addAction(d->A(QA::ItemUp));
  Menu->addAction(d->A(QA::ItemDown));
  Menu->addSeparator();
  QMenu * AllMenu = Menu->addMenu(QStringLiteral("&All"));
  AllMenu->addAction(d->A(QA::PauseAll));
  AllMenu->addAction(d->A(QA::ResumeAll));
  AllMenu->addAction(d->A(QA::DeleteAll));
  AllMenu->addSeparator();
  AllMenu->addAction(d->A(QA::DeleteAllDone));
}

bool QueuePanel::ShouldBeVisible() const
{
  switch (Configuration::Instance().QueueViewShow)
  {
    case qvShow:
      return true;
    case qvHideWhenEmpty:
      return d->Queue && !d->Queue->IsEmpty();
    default:
      return false;
  }
}

void QueuePanel::ToggleVisibility()
{
  // TCustomScpExplorerForm::ToggleQueueVisibility
  Configuration & Config = Configuration::Instance();
  TQueueViewShow LastHideShow = static_cast<TQueueViewShow>(
    ReadQueueViewInt(QStringLiteral("LastHideShow"), qvHideWhenEmpty));
  TQueueViewShow Show = Config.QueueViewShow;
  switch (Config.QueueViewShow)
  {
    case qvShow:
      Show = (d->Queue && !d->Queue->IsEmpty()) ? qvHide : LastHideShow;
      break;
    case qvHideWhenEmpty:
      if (isVisible())
      {
        Show = qvHide;
      }
      else
      {
        LastHideShow = Config.QueueViewShow;
        Show = qvShow;
      }
      break;
    case qvHide:
      LastHideShow = Config.QueueViewShow;
      Show = qvShow;
      break;
  }
  WriteQueueViewInt(QStringLiteral("LastHideShow"), LastHideShow);
  Config.QueueViewShow = Show;
  d->ApplyVisibility();
}

void QueuePanel::ShowUserActionItem(int Id)
{
  if (!d->Queue)
  {
    return;
  }
  d->Refresh();
  for (int Index = 0; Index < d->View->topLevelItemCount(); Index++)
  {
    QTreeWidgetItem * Row = d->View->topLevelItem(Index);
    if (Row->data(0, IdRole).toInt() == Id)
    {
      d->View->setCurrentItem(Row);
      Row->setSelected(true);
      d->View->scrollToItem(Row);
      break;
    }
  }
  d->UpdateActions();
  d->CheckAutoPopup();
}

void QueuePanel::GoTo()
{
  // TCustomScpExplorerForm::GoToQueue (enabled only while the list is visible)
  if (isVisible() && d->View->isEnabled())
  {
    d->View->setFocus();
    if ((d->View->currentItem() == nullptr) && (d->View->topLevelItemCount() > 0))
    {
      d->View->setCurrentItem(d->View->topLevelItem(0));
    }
  }
}

QueuePanel::OnceEmptyOperation QueuePanel::OnceEmpty() const
{
  return d->OnceEmpty;
}

void QueuePanel::SetOnceEmpty(OnceEmptyOperation Operation)
{
  // TNonVisualDataModule::SetQueueOnceEmptyAction
  d->OnceEmpty = Operation;
  d->UpdateActions();
}

int QueuePanel::ConfiguredHeight()
{
  int Height = ReadQueueViewInt(QStringLiteral("Height"), DefaultQueueHeight);
  int PixelsPerInch = ReadQueueViewInt(QStringLiteral("HeightPixelsPerInch"), 96);
  if (PixelsPerInch <= 0)
  {
    PixelsPerInch = 96;
  }
  return std::max(48, Height * 96 / PixelsPerInch);
}

bool QueuePanel::event(QEvent * Event)
{
  bool Result = QWidget::event(Event);
  if ((Event->type() == QEvent::ParentChange) || (Event->type() == QEvent::Show))
  {
    d->InstallWindowShortcuts();
  }
  if ((Event->type() == QEvent::Show) || (Event->type() == QEvent::Hide))
  {
    d->UpdateActions();
  }
  return Result;
}

bool QueuePanel::eventFilter(QObject * Object, QEvent * Event)
{
  if (Object == d->View)
  {
    // TQueueController::QueueViewKeyDown: Enter = default operation, Delete = cancel;
    // the list takes them before the window shortcuts (Delete = delete files)
    if ((Event->type() == QEvent::ShortcutOverride) || (Event->type() == QEvent::KeyPress))
    {
      QKeyEvent * Key = static_cast<QKeyEvent *>(Event);
      bool Handled = (Key->modifiers() == Qt::NoModifier) || (Key->modifiers() == Qt::KeypadModifier);
      Handled = Handled && ((Key->key() == Qt::Key_Return) || (Key->key() == Qt::Key_Enter) || (Key->key() == Qt::Key_Delete));
      if (Handled)
      {
        if (Event->type() == QEvent::ShortcutOverride)
        {
          Event->accept();
          return true;
        }
        if (Key->key() == Qt::Key_Delete)
        {
          d->A(QueueAction::ItemDelete)->trigger();
        }
        else
        {
          d->DefaultOperation();
        }
        return true;
      }
    }
  }
  else if ((Object == d->Splitter->handle(1)) && (Event->type() == QEvent::MouseButtonDblClick))
  {
    // QueueFileListSplitter: double click hides the file list
    d->A(QueueAction::FileList)->trigger();
    return true;
  }
  return QWidget::eventFilter(Object, Event);
}

void QueuePanel::resizeEvent(QResizeEvent * Event)
{
  QWidget::resizeEvent(Event);
  if (isVisible() && (height() > 0))
  {
    // QueueView.Height (restored by the main window's splitter)
    WriteQueueViewInt(QStringLiteral("Height"), height());
    WriteQueueViewInt(QStringLiteral("HeightPixelsPerInch"), 96);
  }
}

QueuePanel::~QueuePanel()
{
  // Disconnect application-wide signals before child widgets are destroyed
  // (focusChanged fires while focused children go away on real displays)
  QObject::disconnect(qApp, nullptr, d, nullptr);
  QObject::disconnect(qApp, nullptr, this, nullptr);
}
