// Port of WinSCP source/forms/Progress.{cpp,dfm} (TProgressForm)
// (FormatBytes from source/packages/filemng/BaseUtils.pas, FormatDateTimeSpan from
// source/core/Common.cpp, ProgressStr/IsIndeterminate from core/FileOperationProgress.cpp,
// OperationComplete and the progress part of the application title from
// forms/CustomScpExplorer.cpp and windows/TerminalManager.cpp)
#include "gui/ProgressDialog.h"
#include "gui/HelpSystem.h"

#include "core/Configuration.h"
#include "core/CopyParam.h"
#include "core/FileSystem.h"
#include "core/IniStorage.h"
#include "gui/Icons.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QElapsedTimer>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMetaMethod>
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QProgressBar>
#include <QShortcut>
#include <QStyle>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <cmath>
#include <cstring>
#include <deque>
#include <limits>

using namespace SystemActions;

namespace
{

// TPathLabel: single line label eliding the middle of the path
class PathLabel : public QLabel
{
public:
  PathLabel() : QLabel()
  {
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    setTextFormat(Qt::PlainText);
  }

  void SetPath(const QString & Path)
  {
    if (FPath != Path)
    {
      FPath = Path;
      setToolTip(Path);
      update();
    }
  }

  QString Path() const { return FPath; }

  QSize sizeHint() const override
  {
    return QSize(100, fontMetrics().height());
  }

  QSize minimumSizeHint() const override
  {
    return QSize(20, fontMetrics().height());
  }

protected:
  void paintEvent(QPaintEvent *) override
  {
    QPainter Painter(this);
    QRect R = contentsRect();
    QString Text = fontMetrics().elidedText(FPath, Qt::ElideMiddle, R.width());
    Painter.setPen(palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled, QPalette::WindowText));
    Painter.drawText(R, Qt::AlignLeft | Qt::AlignVCenter, Text);
  }

private:
  QString FPath;
};

const QString TitleSeparator = QStringLiteral(" - ");
const int MaxSpeedHistory = 10;
const int WheelDelta = 120;   // WHEEL_DELTA

QString FormatBytes(qint64 Bytes)
{
  // BaseUtils.FormatBytes, fbShort style
  QLocale Locale;
  if (Bytes < 1024)
  {
    return QStringLiteral("%1 B").arg(Locale.toString(Bytes));
  }
  double Order;
  QString SizeUnit;
  if (Bytes < 1000LL * 1024)
  {
    Order = 1024.0;
    SizeUnit = QStringLiteral("KB");
  }
  else if (Bytes < 1000LL * 1024 * 1024)
  {
    Order = 1024.0 * 1024;
    SizeUnit = QStringLiteral("MB");
  }
  else
  {
    Order = 1024.0 * 1024 * 1024;
    SizeUnit = QStringLiteral("GB");
  }
  double Value = Bytes / Order;
  QString Number;
  if (Value >= 100)
  {
    Number = Locale.toString(static_cast<qint64>(std::floor(Value)));
  }
  else if (Value >= 10)
  {
    Number = Locale.toString(std::floor(Value * 10) / 10, 'f', 1);
  }
  else
  {
    Number = Locale.toString(std::floor(Value * 100) / 100, 'f', 2);
  }
  return QStringLiteral("%1 %2").arg(Number, SizeUnit);
}

QString FormatTimeSpan(qint64 Seconds)
{
  // FormatDateTimeSpan
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

// CustomWinConfiguration->History["SpeedLimit"] (Configuration\History\SpeedLimit),
// CopySpeedLimits: "Unlimited" first, invalid entries dropped
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
    // TCustomWinConfiguration::DefaultHistory
    Result.append(SetSpeedLimit(0));
    for (unsigned long Speed = MaxSpeed; Speed >= MinSpeed; Speed /= 2)
    {
      Result.append(SetSpeedLimit(Speed));
    }
  }
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
    else if (Valid && (Speed > 0) && !Speeds.contains(Text))
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

void SaveSpeedHistory(const QStringList & Items)
{
  IniStorage Storage(&Configuration::Instance().Ini());
  if (Storage.OpenSubKey(QStringLiteral("Configuration"), true) &&
      Storage.OpenSubKey(QStringLiteral("History"), true) &&
      Storage.OpenSubKey(QStringLiteral("SpeedLimit"), true))
  {
    for (const QString & Name : Storage.GetValueNames())
    {
      Storage.DeleteValue(Name);
    }
    for (int Index = 0; Index < Items.size(); Index++)
    {
      Storage.WriteString(QString::number(Index), Items[Index]);
    }
  }
}

// GUIConfiguration->IgnoreCancelBeforeFinish (Configuration\Interface, TDateTime,
// default 3 s), in milliseconds
qint64 IgnoreCancelBeforeFinishMs()
{
  double Days = 3.0 / 86400;
  IniStorage Storage(&Configuration::Instance().Ini());
  if (Storage.OpenSubKey(QStringLiteral("Configuration"), false) &&
      Storage.OpenSubKey(QStringLiteral("Interface"), false))
  {
    QString Value = Storage.ReadStringRaw(QStringLiteral("IgnoreCancelBeforeFinish"), QString());
    QByteArray Raw = QByteArray::fromHex(Value.toLatin1());
    double Stored;
    if (Raw.size() == static_cast<int>(sizeof(Stored)))
    {
      std::memcpy(&Stored, Raw.constData(), sizeof(Stored));
      Days = Stored;
    }
  }
  return static_cast<qint64>(std::llround(Days * 86400 * 1000));
}

bool IsTransfer(const FileOperationProgress::Snapshot & Data)
{
  return (Data.Op == FileOperationProgress::Copy) || (Data.Op == FileOperationProgress::Move);
}

// TFileOperationProgressType::IsIndeterminate (TotalSizeSet ~ TotalSize > 0)
bool IsIndeterminate(const FileOperationProgress::Snapshot & Data)
{
  bool TotalSizeSet = IsTransfer(Data) && (Data.TotalSize > 0);
  return (Data.Op == FileOperationProgress::Calculate) || (!TotalSizeSet && (Data.Count <= 1));
}

// TFileOperationProgressType::OverallProgress
int OverallProgress(const FileOperationProgress::Snapshot & Data)
{
  if (IsTransfer(Data) && (Data.TotalSize > 0))
  {
    return static_cast<int>(qBound<qint64>(0LL, Data.TotalTransferred * 100 / Data.TotalSize, 100LL));
  }
  return (Data.Count > 0) ? qBound(0, Data.FilesFinished * 100 / Data.Count, 100) : 0;
}

}

struct ProgressDialog::Private
{
  FileOperationProgress * Progress = nullptr;
  QTimer UpdateTimer;
  QElapsedTimer ShowTimer;
  bool Running = false;
  bool DataReceived = false;
  CancelStatus Cancel = csContinue;
  bool PendingCancelAfterFile = false;   // csCancel requested during a file transfer
  QString CancelFileName;
  int CancelFilesFinished = 0;
  bool MoveToQueue = false;
  bool AllowMoveToQueueSet = false;
  bool AllowMoveToQueue = false;
  bool AllowSkip = true;
  bool PendingSkip = false;
  bool Prompting = false;
  bool MinimizedByMe = false;
  bool HoldsInhibitor = false;
  SystemActions::OnceDoneOperation OnceDone = odoIdle;
  QString Target;
  QString ProgressStr;
  QDateTime StartTime;
  qint64 ElapsedMs = 0;
  int WheelDelta = 0;
  unsigned long CPSLimit = 0;
  bool CPSLimitInitialized = false;
  qint64 LastCPS = 0;
  // owner window title mirroring (when no ProgressTitleChanged receiver)
  QPointer<QWidget> Owner;
  QString OwnerBaseTitle;
  QString OwnerSetTitle;

  // samples for CPS (TFileOperationProgressType::CPS uses recent history too)
  struct Sample
  {
    qint64 ElapsedMs;
    qint64 Transferred;
  };
  std::deque<Sample> Samples;

  FrameAnimation * Animation = nullptr;
  QLabel * AnimationImage = nullptr;
  QString AnimationName;
  QLabel * OperationLabel = nullptr;
  QLabel * PathLabelLabel = nullptr;
  PathLabel * FileLabel = nullptr;
  QLabel * TargetLabel = nullptr;
  PathLabel * TargetPathLabel = nullptr;
  QProgressBar * OperationProgress = nullptr;
  QWidget * TransferPanel = nullptr;
  QLabel * TimeLeftLabelLabel = nullptr;
  QLabel * TimeLeftLabel = nullptr;
  QLabel * StartTimeLabelLabel = nullptr;
  QLabel * StartTimeLabel = nullptr;
  QLabel * TimeElapsedLabel = nullptr;
  QLabel * BytesTransferredLabel = nullptr;
  QLabel * CPSLabel = nullptr;
  QProgressBar * FileProgress = nullptr;

  QToolBar * Toolbar = nullptr;
  QAction * CancelItem = nullptr;
  QAction * SkipItem = nullptr;
  QAction * MinimizeItem = nullptr;
  QAction * MoveToQueueItem = nullptr;
  QToolButton * CycleOnceDoneButton = nullptr;
  QAction * CycleOnceDoneItem = nullptr;
  QMenu * OnceDoneMenu = nullptr;
  QAction * OnceDoneItems[4] = {};
  QAction * SpeedItem = nullptr;
  QComboBox * SpeedCombo = nullptr;

  static QString OperationName(const FileOperationProgress::Snapshot & Data);
  qint64 CPS(const FileOperationProgress::Snapshot & Data);
};

QString ProgressDialog::Private::OperationName(const FileOperationProgress::Snapshot & Data)
{
  // TProgressForm::ProgressStr captions
  switch (Data.Op)
  {
    case FileOperationProgress::Copy:
    case FileOperationProgress::Move:
      return (Data.Dir == FileOperationProgress::ToRemote) ? QStringLiteral("Uploading") : QStringLiteral("Downloading");
    case FileOperationProgress::Delete:
      return QStringLiteral("Deleting");
    case FileOperationProgress::SetProperties:
      return QStringLiteral("Setting properties");
    case FileOperationProgress::Calculate:
      return QStringLiteral("Calculating directory size");
  }
  return QString();
}

qint64 ProgressDialog::Private::CPS(const FileOperationProgress::Snapshot & Data)
{
  if (!Samples.empty() && (Data.ElapsedMs < Samples.back().ElapsedMs))
  {
    Samples.clear(); // operation restarted
  }
  Samples.push_back(Sample{Data.ElapsedMs, Data.TotalTransferred});
  // keep about 5 seconds of history
  while ((Samples.size() > 2) && (Data.ElapsedMs - Samples.front().ElapsedMs > 5000))
  {
    Samples.pop_front();
  }
  qint64 Ms = Data.ElapsedMs - Samples.front().ElapsedMs;
  qint64 Bytes = Data.TotalTransferred - Samples.front().Transferred;
  if (Ms < 500)
  {
    // too little history, use the overall average
    Ms = Data.ElapsedMs;
    Bytes = Data.TotalTransferred;
  }
  return (Ms > 0) ? (Bytes * 1000 / Ms) : 0;
}

ProgressDialog::ProgressDialog(FileOperationProgress & Progress, QWidget * Parent) :
  QDialog(Parent),
  d(new Private)
{
  d->Progress = &Progress;
  setWindowTitle(QStringLiteral("Operation"));
  setModal(false);
  setWindowFlag(Qt::WindowContextHelpButtonHint, false);
  // F1 (Progress.dfm has no Help button)
  HelpSystem::InstallFormHelp(this, QStringLiteral("ui_progress"));

  d->Animation = new FrameAnimation();
  d->AnimationImage = new QLabel();
  d->AnimationImage->setFixedSize(32, 32);
  d->AnimationImage->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
  d->AnimationImage->hide();

  d->OperationLabel = new QLabel();
  QFont BoldFont = d->OperationLabel->font();
  BoldFont.setBold(true);
  d->OperationLabel->setFont(BoldFont);

  // MainPanel
  auto * MainGrid = new QGridLayout();
  MainGrid->setContentsMargins(0, 0, 0, 0);
  MainGrid->setVerticalSpacing(3);
  d->PathLabelLabel = new QLabel(QStringLiteral("File:"));
  d->FileLabel = new PathLabel();
  d->TargetLabel = new QLabel(QStringLiteral("Target:"));
  d->TargetPathLabel = new PathLabel();
  d->TargetLabel->hide();
  d->TargetPathLabel->hide();
  d->OperationProgress = new QProgressBar();
  d->OperationProgress->setRange(0, 100);
  d->OperationProgress->setTextVisible(false);
  d->OperationProgress->setFixedHeight(16);
  MainGrid->addWidget(d->OperationLabel, 0, 0, 1, 2);
  MainGrid->addWidget(d->PathLabelLabel, 1, 0);
  MainGrid->addWidget(d->FileLabel, 1, 1);
  MainGrid->addWidget(d->TargetLabel, 2, 0);
  MainGrid->addWidget(d->TargetPathLabel, 2, 1);
  MainGrid->addWidget(d->OperationProgress, 3, 0, 1, 2);
  MainGrid->setColumnMinimumWidth(0, 56);
  MainGrid->setColumnStretch(1, 1);

  // TransferPanel
  d->TransferPanel = new QWidget();
  auto * TransferGrid = new QGridLayout(d->TransferPanel);
  TransferGrid->setContentsMargins(0, 0, 0, 0);
  TransferGrid->setVerticalSpacing(3);
  d->TimeLeftLabelLabel = new QLabel(QStringLiteral("Time left:"));
  d->TimeLeftLabel = new QLabel(QStringLiteral("0:00:00"));
  d->StartTimeLabelLabel = new QLabel(QStringLiteral("Start time:"));
  d->StartTimeLabel = new QLabel(QStringLiteral("0:00:00"));
  d->TimeElapsedLabel = new QLabel(QStringLiteral("0:00:00"));
  d->BytesTransferredLabel = new QLabel(QStringLiteral("0 B"));
  d->CPSLabel = new QLabel(QStringLiteral("0 B/s"));
  d->TimeElapsedLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  d->CPSLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  TransferGrid->addWidget(d->TimeLeftLabelLabel, 0, 0);
  TransferGrid->addWidget(d->TimeLeftLabel, 0, 1);
  // StartTimeLabel takes the place of TimeLeftLabel when the total size is not known
  TransferGrid->addWidget(d->StartTimeLabelLabel, 0, 0);
  TransferGrid->addWidget(d->StartTimeLabel, 0, 1);
  d->StartTimeLabelLabel->hide();
  d->StartTimeLabel->hide();
  TransferGrid->addWidget(new QLabel(QStringLiteral("Time elapsed:")), 0, 2);
  TransferGrid->addWidget(d->TimeElapsedLabel, 0, 3);
  TransferGrid->addWidget(new QLabel(QStringLiteral("Bytes transferred:")), 1, 0);
  TransferGrid->addWidget(d->BytesTransferredLabel, 1, 1);
  TransferGrid->addWidget(new QLabel(QStringLiteral("Speed:")), 1, 2);
  TransferGrid->addWidget(d->CPSLabel, 1, 3);
  d->FileProgress = new QProgressBar();
  d->FileProgress->setRange(0, 100);
  d->FileProgress->setTextVisible(false);
  d->FileProgress->setFixedHeight(16);
  TransferGrid->addWidget(d->FileProgress, 2, 0, 1, 4);
  TransferGrid->setColumnStretch(1, 1);
  TransferGrid->setColumnMinimumWidth(1, 70);
  TransferGrid->setColumnMinimumWidth(3, 74);

  // Toolbar (Progress.dfm:225-297)
  d->Toolbar = new QToolBar();
  // icons with hints, only Cancel shows its caption (as the TBX toolbar of WinSCP)
  d->Toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
  d->Toolbar->setIconSize(QSize(16, 16));
  d->Toolbar->setFloatable(false);
  d->Toolbar->setMovable(false);

  d->CancelItem = d->Toolbar->addAction(Icons::Get(QStringLiteral("queue-delete")), QStringLiteral("&Cancel"));
  // Esc is handled by reject()
  d->SkipItem = d->Toolbar->addAction(
    Icons::FromTheme(QStringLiteral("media-skip-forward go-next")), QStringLiteral("Skip this file"));
  d->SkipItem->setToolTip(QStringLiteral("Skip this file"));
  d->MinimizeItem = d->Toolbar->addAction(
    Icons::FromTheme(QStringLiteral("window-minimize go-down")), QStringLiteral("&Minimize"));
  d->MinimizeItem->setShortcut(QKeySequence(Qt::ALT | Qt::Key_M));
  d->MoveToQueueItem = d->Toolbar->addAction(Icons::Get(QStringLiteral("upload-queue")), QStringLiteral("Proceed in &Background"));
  d->MoveToQueueItem->setShortcut(QKeySequence(Qt::ALT | Qt::Key_B));
  d->MoveToQueueItem->setVisible(false);
  d->MinimizeItem->setToolTip(QStringLiteral("Minimize (Alt+M)"));
  d->MoveToQueueItem->setToolTip(QStringLiteral("Proceed in Background (Alt+B)"));
  if (auto * CancelButton = qobject_cast<QToolButton *>(d->Toolbar->widgetForAction(d->CancelItem)))
  {
    CancelButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  }

  // CycleOnceDoneItem: dropdown combo, click cycles, menu selects
  d->OnceDoneMenu = new QMenu(this);
  auto * OnceDoneGroup = new QActionGroup(this);
  for (int Index = odoIdle; Index <= odoShutDown; Index++)
  {
    auto Operation = static_cast<SystemActions::OnceDoneOperation>(Index);
    QAction * Item = d->OnceDoneMenu->addAction(Icons::Get(OnceDoneIconName(Operation)), OnceDoneCaption(Operation));
    Item->setCheckable(true);
    Item->setData(Index);
    OnceDoneGroup->addAction(Item);
    d->OnceDoneItems[Index] = Item;
    connect(Item, &QAction::triggered, this, [this, Operation]() { SetOnceDoneOperation(Operation); });
  }
  d->OnceDoneItems[odoIdle]->setChecked(true);
  d->OnceDoneItems[odoSuspend]->setVisible(OperationSupported(odoSuspend));
  d->OnceDoneItems[odoShutDown]->setVisible(OperationSupported(odoShutDown));
  d->CycleOnceDoneButton = new QToolButton();
  d->CycleOnceDoneButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
  d->CycleOnceDoneButton->setPopupMode(QToolButton::MenuButtonPopup);
  d->CycleOnceDoneButton->setMenu(d->OnceDoneMenu);
  d->CycleOnceDoneButton->setText(QStringLiteral("Once Finished"));
  d->CycleOnceDoneButton->setToolTip(QStringLiteral("Action to perform once the operation finishes"));
  d->CycleOnceDoneButton->setIcon(Icons::Get(OnceDoneIconName(odoIdle)));
  d->CycleOnceDoneItem = d->Toolbar->addWidget(d->CycleOnceDoneButton);
  connect(d->CycleOnceDoneButton, &QToolButton::clicked, this, [this]()
  {
    // CycleOnceDoneItemClick: next visible item
    int Index = d->OnceDone;
    for (int Step = 0; Step < 4; Step++)
    {
      Index = (Index + 1) % 4;
      if (d->OnceDoneItems[Index]->isVisible())
      {
        break;
      }
    }
    SetOnceDoneOperation(static_cast<SystemActions::OnceDoneOperation>(Index));
  });
  auto * CycleShortcut = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_F), this);
  connect(CycleShortcut, &QShortcut::activated, d->CycleOnceDoneButton, &QToolButton::click);

  // SpeedComboBoxItem
  auto * SpeedPanel = new QWidget();
  auto * SpeedLayout = new QHBoxLayout(SpeedPanel);
  SpeedLayout->setContentsMargins(4, 0, 0, 0);
  SpeedLayout->setSpacing(2);
  auto * SpeedIcon = new QLabel();
  SpeedIcon->setPixmap(Icons::Get(QStringLiteral("speed-limit")).pixmap(16, 16));
  d->SpeedCombo = new QComboBox();
  d->SpeedCombo->setEditable(true);
  d->SpeedCombo->setInsertPolicy(QComboBox::NoInsert);
  d->SpeedCombo->setFixedWidth(110);
  d->SpeedCombo->setToolTip(QStringLiteral("Speed limit (KB/s)"));
  d->SpeedCombo->setFocusPolicy(Qt::ClickFocus);
  d->SpeedCombo->installEventFilter(this);
  d->SpeedCombo->lineEdit()->installEventFilter(this);
  SpeedLayout->addWidget(SpeedIcon);
  SpeedLayout->addWidget(d->SpeedCombo);
  SpeedIcon->setToolTip(d->SpeedCombo->toolTip());
  d->SpeedItem = d->Toolbar->addWidget(SpeedPanel);
  auto * SpeedShortcut = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_S), this);
  connect(SpeedShortcut, &QShortcut::activated, this, [this]()
  {
    if (d->SpeedItem->isVisible())
    {
      d->SpeedCombo->setFocus(Qt::ShortcutFocusReason);
      d->SpeedCombo->lineEdit()->selectAll();
    }
  });
  connect(d->SpeedCombo, &QComboBox::activated, this, [this](int Index)
  {
    ItemSpeed(d->SpeedCombo->itemText(Index));
  });
  connect(d->SpeedCombo->lineEdit(), &QLineEdit::returnPressed, this, [this]()
  {
    ItemSpeed(d->SpeedCombo->currentText());
  });

  auto * Content = new QVBoxLayout();
  Content->addLayout(MainGrid);
  Content->addWidget(d->TransferPanel);
  Content->addSpacing(4);
  Content->addWidget(d->Toolbar);

  auto * AnimationColumn = new QVBoxLayout();
  AnimationColumn->addWidget(d->Animation);
  AnimationColumn->addWidget(d->AnimationImage);
  AnimationColumn->addStretch();

  auto * Layout = new QHBoxLayout(this);
  Layout->addLayout(AnimationColumn);
  Layout->addLayout(Content, 1);

  setMinimumWidth(398);
  resize(460, sizeHint().height());

  connect(d->CancelItem, &QAction::triggered, this, &ProgressDialog::CancelOperation);
  connect(d->SkipItem, &QAction::triggered, this, [this]() { SetCancelLower(csCancelFile); });
  connect(d->MinimizeItem, &QAction::triggered, this, &ProgressDialog::Minimize);
  connect(d->MoveToQueueItem, &QAction::triggered, this, [this]()
  {
    // MoveToQueueItemClick: the operation stops after the current file (csCancel)
    if (d->MoveToQueue || (d->Cancel != csContinue))
    {
      return;
    }
    d->MoveToQueue = true;
    FileOperationProgress::Snapshot Data = d->Progress->Get();
    if (IsTransfer(Data) && (Data.FileSize > 0) && (Data.FileTransferred < Data.FileSize))
    {
      d->PendingCancelAfterFile = true;
      d->CancelFileName = Data.FileName;
      d->CancelFilesFinished = Data.FilesFinished;
    }
    else
    {
      d->Progress->Cancel();
    }
    UpdateControls();
    emit MoveToQueueRequested();
  });

  d->UpdateTimer.setInterval(100);
  connect(&d->UpdateTimer, &QTimer::timeout, this, [this]()
  {
    if (!d->HoldsInhibitor && d->ShowTimer.isValid() && (d->ShowTimer.elapsed() >= 500))
    {
      // TFileOperationProgressType::SystemRequired
      SleepInhibitor::Acquire();
      d->HoldsInhibitor = true;
    }
    if (!isVisible())
    {
      FileOperationProgress::Snapshot Data = d->Progress->Get();
      bool OwnerMinimized = (d->Owner != nullptr) && d->Owner->isMinimized();
      if (Data.InProgress && d->ShowTimer.isValid() && (d->ShowTimer.elapsed() >= 500) && !OwnerMinimized)
      {
        if (!d->CPSLimitInitialized)
        {
          // SpeedComboBoxItem->Text = SetSpeedLimit(FCPSLimit) when showing
          d->CPSLimit = d->Progress->CPSLimit();
          d->SpeedCombo->setEditText(SetSpeedLimit(d->CPSLimit));
          d->CPSLimitInitialized = true;
        }
        d->DataReceived = true;
        show();
      }
      else
      {
        // still mirror the progress in the main window title
        if (Data.InProgress && d->ShowTimer.isValid() && (d->ShowTimer.elapsed() >= 500))
        {
          d->DataReceived = true;
          UpdateControls();
        }
        return;
      }
    }
    UpdateControls();
  });
}

ProgressDialog::~ProgressDialog()
{
  if (d->Prompting)
  {
    d->Progress->Resume();
  }
  if (d->Running)
  {
    SetOwnerTitle(QString());
  }
  if (d->HoldsInhibitor)
  {
    SleepInhibitor::Release();
  }
  delete d;
}

void ProgressDialog::UpdateControls()
{
  FileOperationProgress::Snapshot Data = d->Progress->Get();
  d->ElapsedMs = Data.ElapsedMs;
  if (!d->StartTime.isValid() && Data.InProgress)
  {
    d->StartTime = QDateTime::currentDateTime().addMSecs(-Data.ElapsedMs);
  }

  bool Transfer = IsTransfer(Data);
  bool Indeterminate = IsIndeterminate(Data);
  bool TotalSizeSet = Transfer && (Data.TotalSize > 0);

  // pending "cancel after the current file" (csCancel during a transfer,
  // also used by Proceed in Background): abort as soon as the file is done
  if (d->PendingCancelAfterFile &&
      ((Data.FileName != d->CancelFileName) || (Data.FilesFinished != d->CancelFilesFinished) || !Data.InProgress))
  {
    d->PendingCancelAfterFile = false;
    d->Progress->Cancel();
  }

  d->PathLabelLabel->setText((Data.Op == FileOperationProgress::Calculate) ? QStringLiteral("Path:") : QStringLiteral("File:"));
  if (d->FileLabel->Path() != Data.FileName)
  {
    d->FileLabel->SetPath(Data.FileName);
    d->PendingSkip = false;
  }

  // UpdateControls: item states
  d->CancelItem->setEnabled(d->Cancel < csCancel);
  d->SkipItem->setVisible(Transfer && d->AllowSkip);
  d->SkipItem->setEnabled((d->Cancel < csCancelFile) && !d->PendingSkip && !d->MoveToQueue);
  d->MoveToQueueItem->setEnabled(!d->MoveToQueue && (d->Cancel == csContinue) && !d->PendingSkip);
  bool HideOnceDone = (Data.Op == FileOperationProgress::Calculate);
  d->CycleOnceDoneItem->setVisible(!HideOnceDone);
  d->SpeedItem->setVisible(Transfer);
  // Wider fonts or longer translations must not push items (the speed
  // combo) into the toolbar's overflow menu: the dialog grows instead
  d->Toolbar->setMinimumWidth(0);
  d->Toolbar->setMinimumWidth(d->Toolbar->sizeHint().width());

  QString AnimationName =
    (Data.Op == FileOperationProgress::Delete) ? QStringLiteral("Delete") :
    (Data.Op == FileOperationProgress::SetProperties) ? QStringLiteral("SetProperties") :
    (Data.Op == FileOperationProgress::Calculate) ? QStringLiteral("CalculateSize") :
    (Data.Op == FileOperationProgress::Move) ?
      ((Data.Dir == FileOperationProgress::ToRemote) ? QStringLiteral("MoveLocal") : QStringLiteral("MoveRemote")) :
      ((Data.Dir == FileOperationProgress::ToRemote) ? QStringLiteral("CopyLocal") : QStringLiteral("CopyRemote"));
  if (AnimationName != d->AnimationName)
  {
    d->AnimationName = AnimationName;
    bool HasFrames = !Icons::AnimationFrames(AnimationName).isEmpty();
    d->Animation->setVisible(HasFrames);
    d->AnimationImage->setVisible(!HasFrames);
    if (HasFrames)
    {
      d->Animation->Init(AnimationName);
      d->Animation->Start();
    }
    else
    {
      d->AnimationImage->setPixmap(style()->standardIcon(
        (Data.Op == FileOperationProgress::Delete) ? QStyle::SP_TrashIcon :
        !Transfer ? QStyle::SP_FileDialogDetailedView :
        (Data.Dir == FileOperationProgress::ToRemote) ? QStyle::SP_ArrowUp : QStyle::SP_ArrowDown).pixmap(32, 32));
    }
    d->MoveToQueueItem->setIcon(Icons::Get(
      (Data.Dir == FileOperationProgress::ToRemote) ? QStringLiteral("upload-queue") : QStringLiteral("download-queue")));
  }

  d->StartTimeLabelLabel->setVisible(Transfer && !TotalSizeSet);
  d->StartTimeLabel->setVisible(Transfer && !TotalSizeSet);
  d->TimeLeftLabelLabel->setVisible(!Transfer || TotalSizeSet);
  d->TimeLeftLabel->setVisible(!Transfer || TotalSizeSet);


  int Overall = Indeterminate ? -1 : OverallProgress(Data);
  if (Indeterminate)
  {
    // marquee (pbstMarquee)
    d->OperationProgress->setRange(0, 0);
  }
  else
  {
    d->OperationProgress->setRange(0, 100);
    d->OperationProgress->setValue(Overall);
  }
  d->OperationProgress->setToolTip((Overall >= 0) ? QStringLiteral("%1%").arg(Overall) : QString());

  // ProgressStr / FormatFormCaption
  QString Caption = Private::OperationName(Data);
  d->ProgressStr = (Overall >= 0) ? QStringLiteral("%1% %2").arg(Overall).arg(Caption) : Caption;
  setWindowTitle(d->ProgressStr);
  if ((d->Cancel < csCancel) && !d->PendingCancelAfterFile)
  {
    d->OperationLabel->setText(Caption);
  }
  SetOwnerTitle(d->ProgressStr);

  bool ShowTarget = Transfer && !d->Target.isEmpty();
  d->TargetLabel->setVisible(ShowTarget);
  d->TargetPathLabel->setVisible(ShowTarget);
  d->TargetPathLabel->SetPath(d->Target);

  d->TransferPanel->setVisible(Transfer);
  if (Transfer)
  {
    qint64 CPS = d->CPS(Data);
    d->LastCPS = CPS;
    d->StartTimeLabel->setText(QLocale().toString(d->StartTime.time(), QLocale::LongFormat));
    if (TotalSizeSet)
    {
      // CanShowTimeEstimate: after 3 seconds
      if ((Data.ElapsedMs >= 3000) && (CPS > 0))
      {
        qint64 Remaining = qMax<qint64>(0, Data.TotalSize - Data.TotalTransferred);
        d->TimeLeftLabel->setText(FormatTimeSpan((Remaining + CPS - 1) / CPS));
      }
      else
      {
        d->TimeLeftLabel->setText(QStringLiteral("Calculating"));
      }
    }
    d->TimeElapsedLabel->setText(FormatTimeSpan(Data.ElapsedMs / 1000));
    d->BytesTransferredLabel->setText(FormatBytes(Data.TotalTransferred));
    d->CPSLabel->setText(QStringLiteral("%1/s").arg(FormatBytes(CPS)));
    int FilePercent = (Data.FileSize > 0) ?
      static_cast<int>(qBound<qint64>(0LL, Data.FileTransferred * 100 / Data.FileSize, 100LL)) : 0;
    d->FileProgress->setValue(FilePercent);
    d->FileProgress->setToolTip(QStringLiteral("%1%").arg(FilePercent));
  }
}

void ProgressDialog::Start()
{
  d->Cancel = csContinue;
  d->PendingCancelAfterFile = false;
  d->MoveToQueue = false;
  d->PendingSkip = false;
  d->MinimizedByMe = false;
  d->Running = true;
  d->DataReceived = false;
  d->CPSLimitInitialized = false;
  d->StartTime = QDateTime();
  d->Samples.clear();
  d->AnimationName.clear();
  d->ProgressStr.clear();
  d->OperationLabel->clear();
  d->FileLabel->SetPath(QString());
  d->Owner = (parentWidget() != nullptr) ? parentWidget()->window() : nullptr;
  d->OwnerBaseTitle.clear();
  d->OwnerSetTitle.clear();
  d->SpeedCombo->clear();
  d->SpeedCombo->addItems(SpeedHistory());
  if (!d->AllowMoveToQueueSet)
  {
    d->AllowMoveToQueue = isSignalConnected(QMetaMethod::fromSignal(&ProgressDialog::MoveToQueueRequested));
  }
  d->MoveToQueueItem->setVisible(d->AllowMoveToQueue);
  // "Disconnect Session" needs a receiver that can close the session
  bool CanDisconnect = isSignalConnected(QMetaMethod::fromSignal(&ProgressDialog::OnceDoneRequested));
  d->OnceDoneItems[odoDisconnect]->setVisible(CanDisconnect);
  if (!CanDisconnect && (d->OnceDone == odoDisconnect))
  {
    SetOnceDoneOperation(odoIdle);
  }
  // the sleep inhibitor is taken by the update timer once the operation has
  // run for the show delay, so short operations do not talk to logind
  d->ShowTimer.start();
  d->UpdateTimer.start();
}

void ProgressDialog::Finish()
{
  Finish(true);
}

void ProgressDialog::Finish(bool Success)
{
  bool WasRunning = d->Running;
  d->Running = false;
  d->PendingCancelAfterFile = false;
  d->UpdateTimer.stop();
  d->ShowTimer.invalidate();
  d->Animation->Stop();
  hide();
  if (d->HoldsInhibitor)
  {
    SleepInhibitor::Release();
    d->HoldsInhibitor = false;
  }
  if (!WasRunning)
  {
    return;
  }
  d->ProgressStr.clear();
  SetOwnerTitle(QString());

  bool Completed = Success && (d->Cancel == csContinue) && !d->MoveToQueue && !d->Progress->Cancelled();
  if (!Completed)
  {
    return;
  }

  // TCustomScpExplorerForm::OperationComplete
  Configuration & Config = Configuration::Instance();
  if (Config.BeepOnFinish && (d->ElapsedMs > static_cast<qint64>(Config.BeepOnFinishAfter) * 1000))
  {
    // BeepSound (a Windows sound alias/file) has no Linux counterpart; the
    // desktop bell is used
    QApplication::beep();
  }
  if (d->MinimizedByMe && (d->Owner != nullptr) && d->Owner->isMinimized())
  {
    // BALLOON_OPERATION_COMPLETE
    SystemActions::Notify(QStringLiteral("NixSCP"), QStringLiteral("Operation was completed"));
  }

  SystemActions::OnceDoneOperation Operation = d->OnceDone;
  if (Operation != odoIdle)
  {
    // the setting applies to this operation only (ResetOnceDoneOperation)
    SetOnceDoneOperation(odoIdle);
    QPointer<ProgressDialog> Self(this);
    emit OnceDoneRequested(Operation);
    QString Error;
    if (!SystemActions::Execute(Operation, &Error) && !Error.isEmpty())
    {
      QMessageBox::warning(Self ? Self->parentWidget() : nullptr, QStringLiteral("NixSCP"), Error);
    }
  }
}

void ProgressDialog::SetTarget(const QString & Directory)
{
  d->Target = Directory;
}

void ProgressDialog::SetAllowMoveToQueue(bool Allow)
{
  d->AllowMoveToQueueSet = true;
  d->AllowMoveToQueue = Allow;
  d->MoveToQueueItem->setVisible(Allow && d->Running);
}

void ProgressDialog::SetAllowSkip(bool Allow)
{
  d->AllowSkip = Allow;
}

SystemActions::OnceDoneOperation ProgressDialog::OnceDoneOperation() const
{
  return d->OnceDone;
}

void ProgressDialog::SetOnceDoneOperation(SystemActions::OnceDoneOperation Operation)
{
  d->OnceDone = Operation;
  d->OnceDoneItems[Operation]->setChecked(true);
  d->CycleOnceDoneButton->setIcon(Icons::Get(OnceDoneIconName(Operation)));
  QString Caption = OnceDoneCaption(Operation);
  Caption.remove(QLatin1Char('&'));
  d->CycleOnceDoneButton->setToolTip(
    QStringLiteral("Action to perform once the operation finishes") + QStringLiteral(": ") + Caption);
}

ProgressDialog::CancelStatus ProgressDialog::Cancel() const
{
  return d->Cancel;
}

bool ProgressDialog::MoveToQueue() const
{
  return d->MoveToQueue;
}

QString ProgressDialog::ProgressTitle() const
{
  return d->Running ? d->ProgressStr : QString();
}

bool ProgressDialog::MinimizedByMe() const
{
  return d->MinimizedByMe;
}

void ProgressDialog::SetOwnerTitle(const QString & ProgressTitle)
{
  // TTerminalManager::UpdateAppTitle: "<progress> - <title>"
  if (isSignalConnected(QMetaMethod::fromSignal(&ProgressDialog::ProgressTitleChanged)))
  {
    emit ProgressTitleChanged(ProgressTitle);
    return;
  }
  if (d->Owner == nullptr)
  {
    return;
  }
  QString Current = d->Owner->windowTitle();
  if (d->OwnerSetTitle.isEmpty() || (Current != d->OwnerSetTitle))
  {
    // first update, or the owner changed its title meanwhile (directory change)
    d->OwnerBaseTitle = Current;
  }
  if (ProgressTitle.isEmpty())
  {
    if (!d->OwnerSetTitle.isEmpty() && (Current == d->OwnerSetTitle))
    {
      d->Owner->setWindowTitle(d->OwnerBaseTitle);
    }
    d->OwnerSetTitle.clear();
    return;
  }
  QString Title = ProgressTitle + TitleSeparator + d->OwnerBaseTitle;
  if (Title != Current)
  {
    d->Owner->setWindowTitle(Title);
  }
  d->OwnerSetTitle = Title;
}

void ProgressDialog::SetCancelLower(CancelStatus ACancel)
{
  // TProgressForm::SetCancelLower + TCustomScpExplorerForm::FileOperationProgress
  if (d->Cancel >= ACancel)
  {
    return;
  }
  switch (ACancel)
  {
    case csCancelFile:
      // csCancelFile is cleared right after it is handed over (ClearCancel)
      d->Progress->CancelFile();
      d->PendingSkip = true;
      break;
    case csCancel:
    {
      d->Cancel = csCancel;
      FileOperationProgress::Snapshot Data = d->Progress->Get();
      bool TransferringFile = IsTransfer(Data) && (Data.FileSize > 0) && (Data.FileTransferred < Data.FileSize);
      if (TransferringFile)
      {
        // finish the current file, then stop
        d->PendingCancelAfterFile = true;
        d->CancelFileName = Data.FileName;
        d->CancelFilesFinished = Data.FilesFinished;
      }
      else
      {
        d->Progress->Cancel();
      }
      d->MoveToQueue = false;   // cancel cancels even the move
      break;
    }
    case csCancelTransfer:
      d->Cancel = csCancelTransfer;
      d->PendingCancelAfterFile = false;
      d->MoveToQueue = false;
      d->Progress->Cancel();
      break;
    default:
      break;
  }
  if (d->Cancel >= csCancel)
  {
    d->OperationLabel->setText(QStringLiteral("Cancelling..."));
  }
  UpdateControls();
}

void ProgressDialog::CancelOperation()
{
  // TProgressForm::CancelOperation
  if (!d->Running || d->Prompting)
  {
    return;
  }
  FileOperationProgress::Snapshot Data = d->Progress->Get();
  bool TransferringFile = IsTransfer(Data) && (Data.FileSize > 0) && (Data.FileTransferred < Data.FileSize);
  CancelStatus ACancel = csCancel;
  if (TransferringFile)
  {
    // TFileOperationProgressType::TimeExpected for the current file
    qint64 CPS = qMax<qint64>(d->LastCPS, 0);
    qint64 Remaining = Data.FileSize - Data.FileTransferred;
    qint64 ExpectedMs = (CPS > 0) ? (Remaining * 1000 / CPS) : std::numeric_limits<qint64>::max();
    if (ExpectedMs > IgnoreCancelBeforeFinishMs())
    {
      // the worker waits in CheckCancel while the question is shown
      d->Progress->Suspend();
      d->Prompting = true;
      QPointer<ProgressDialog> Self(this);
      auto * Box = new QMessageBox(QMessageBox::Warning, windowTitle(),
        // CANCEL_OPERATION_FATAL2 (Yes cancels the transfer now; NixSCP does
        // not need to close the connection for that)
        QStringLiteral("Cancel file transfer?"),
        QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, this);
      Box->setAttribute(Qt::WA_DeleteOnClose, false);
      Box->setInformativeText(
        QStringLiteral("Operation can't be canceled in the middle of file transfer.\n"
                       "Press 'Yes' to cancel file transfer now.\n"
                       "Press 'No' to finish current file transfer.\n"
                       "Press 'Cancel' to continue operation."));
      Box->setDefaultButton(QMessageBox::Cancel);
      QPointer<QMessageBox> BoxGuard(Box);
      int Result = Box->exec();
      if (!Self)
      {
        // the destructor resumed the worker
        return;
      }
      delete BoxGuard.data();
      d->Prompting = false;
      d->Progress->Resume();
      switch (Result)
      {
        case QMessageBox::Yes: ACancel = csCancelTransfer; break;
        case QMessageBox::No: ACancel = csCancel; break;
        default: ACancel = csContinue; break;
      }
    }
  }
  if (!d->Running)
  {
    return;
  }
  SetCancelLower(ACancel);
}

void ProgressDialog::reject()
{
  // Esc / window close = CancelOperation; the owner calls Finish() once the
  // worker thread has actually aborted
  if (!d->Running)
  {
    hide();
    return;
  }
  CancelOperation();
}

void ProgressDialog::Minimize()
{
  // GlobalMinimize / ApplicationMinimize: the whole application
  for (QWidget * Widget : QApplication::topLevelWidgets())
  {
    if (Widget->isVisible() && Widget->isWindow() && (Widget->windowType() != Qt::Popup) &&
        (Widget->windowType() != Qt::ToolTip))
    {
      Widget->showMinimized();
    }
  }
  d->MinimizedByMe = true;
}

void ProgressDialog::ItemSpeed(const QString & Text)
{
  // TProgressForm::ItemSpeed (keep in sync with QueuePanel)
  unsigned long Speed;
  if (!TryGetSpeedLimit(Text, Speed))
  {
    QMessageBox::warning(this, windowTitle(), QStringLiteral("'%1' is not a valid speed limit.").arg(Text));
    d->SpeedCombo->setEditText(SetSpeedLimit(d->CPSLimit));
    return;
  }
  d->CPSLimit = Speed;
  d->CPSLimitInitialized = true;
  d->Progress->SetCPSLimit(Speed);
  QString Result = SetSpeedLimit(Speed);
  // SaveToHistory: most recent first, no duplicates
  QStringList Items;
  for (int Index = 0; Index < d->SpeedCombo->count(); Index++)
  {
    Items.append(d->SpeedCombo->itemText(Index));
  }
  Items.removeAll(Result);
  Items.prepend(Result);
  while (Items.size() > MaxSpeedHistory)
  {
    Items.removeLast();
  }
  SaveSpeedHistory(Items);
  d->SpeedCombo->blockSignals(true);
  d->SpeedCombo->clear();
  d->SpeedCombo->addItems(SpeedHistory());
  d->SpeedCombo->setEditText(Result);
  d->SpeedCombo->blockSignals(false);
  d->WheelDelta = 0;
}

bool ProgressDialog::eventFilter(QObject * Object, QEvent * Event)
{
  if (((Object == d->SpeedCombo) || (Object == d->SpeedCombo->lineEdit())) && (Event->type() == QEvent::Wheel))
  {
    // TProgressForm::MouseWheelHandler: wheel over the speed combo halves/doubles the limit
    auto * Wheel = static_cast<QWheelEvent *>(Event);
    d->WheelDelta += Wheel->angleDelta().y();
    unsigned long CurrentSpeed;
    if (!TryGetSpeedLimit(d->SpeedCombo->currentText(), CurrentSpeed))
    {
      CurrentSpeed = d->CPSLimit;
    }
    unsigned long Speed = CurrentSpeed;
    unsigned long CPS = static_cast<unsigned long>(qMax<qint64>(d->LastCPS, 0));
    int Step = 4 * WheelDelta;
    if (d->WheelDelta > 0)
    {
      while (d->WheelDelta > Step)
      {
        if (Speed > 0)
        {
          Speed *= 2;
          if (Speed > std::max(MaxSpeed, CPS * 2))
          {
            Speed = 0;
          }
        }
        d->WheelDelta -= Step;
      }
    }
    else if (d->WheelDelta < 0)
    {
      while (d->WheelDelta < -Step)
      {
        if (Speed == 0)
        {
          Speed = 8 * 1024;
          while (Speed * 2 < CPS)
          {
            Speed *= 2;
          }
        }
        else if (Speed > MinSpeed)
        {
          Speed /= 2;
        }
        d->WheelDelta += Step;
      }
    }
    if (Speed != CurrentSpeed)
    {
      d->CPSLimit = Speed;
      d->CPSLimitInitialized = true;
      d->Progress->SetCPSLimit(Speed);
      d->SpeedCombo->setEditText(SetSpeedLimit(Speed));
    }
    return true;
  }
  return QDialog::eventFilter(Object, Event);
}
