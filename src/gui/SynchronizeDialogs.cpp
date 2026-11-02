// Port of WinSCP source/forms/FullSynchronize.{cpp,dfm}, source/forms/SynchronizeChecklist.{cpp,dfm},
// source/forms/SynchronizeProgress.{cpp,dfm}, source/forms/Synchronize.{cpp,dfm} and of
// TCustomScpExplorerForm::DoFullSynchronizeDirectories/FullSynchronize/DoSynchronizeDirectories
// (source/forms/CustomScpExplorer.cpp) and TScpCommanderForm::CompareDirectories.
//
// Not ported: the custom commands of the checklist (written in
// another package). "Start in New Window", "Start in Background" and Explore
// of the remote directory are provided by the main window through
// SynchronizeHooks(); the presets drop-downs use CopyParamListPopup.
#include "gui/SynchronizeDialogs.h"
#include "gui/HelpSystem.h"

#include "core/Configuration.h"
#include "core/IniStorage.h"
#include "core/Queue.h"
#include "gui/CopyParamsDialog.h"
#include "gui/EditMaskDialog.h"
#include "gui/FileListModel.h"
#include "gui/GuiUserInterface.h"
#include "gui/ProgressDialog.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QScreen>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>

#include <map>

namespace
{

const QString InterfaceKey = QStringLiteral("Configuration\\Interface");
const QString ChecklistKey = QStringLiteral("Configuration\\Interface\\SynchronizeChecklist");
// THistoryComboBox HistoryKey of the directory boxes
const QString LocalDirectoryHistory = QStringLiteral("LocalDirectory");
const QString RemoteDirectoryHistory = QStringLiteral("RemoteDirectory");

QString FormatDateTimeSpan(qint64 Ms)
{
  qint64 Seconds = Ms / 1000;
  return QStringLiteral("%1:%2:%3")
    .arg(Seconds / 3600, 2, 10, QLatin1Char('0'))
    .arg((Seconds / 60) % 60, 2, 10, QLatin1Char('0'))
    .arg(Seconds % 60, 2, 10, QLatin1Char('0'));
}

QString FormatModification(const QDateTime & Time)
{
  return Time.isValid() ? QLocale().toString(Time.toLocalTime(), QLocale::ShortFormat) : QString();
}

// Action glyphs (WinSCP ActionImages): arrows point to the target panel
// (local is left, remote right); green = new, blue = update, red cross = delete
QIcon ActionIcon(SynchronizeChecklist::ActionType Action)
{
  static std::map<int, QIcon> Cache;
  auto It = Cache.find(Action);
  if (It != Cache.end())
  {
    return It->second;
  }
  QPixmap Pixmap(16, 16);
  Pixmap.fill(Qt::transparent);
  QPainter Painter(&Pixmap);
  Painter.setRenderHint(QPainter::Antialiasing);
  switch (Action)
  {
    case SynchronizeChecklist::saUploadNew:
    case SynchronizeChecklist::saUploadUpdate:
    case SynchronizeChecklist::saDownloadNew:
    case SynchronizeChecklist::saDownloadUpdate:
      {
        bool Right = (Action == SynchronizeChecklist::saUploadNew) || (Action == SynchronizeChecklist::saUploadUpdate);
        bool New = (Action == SynchronizeChecklist::saUploadNew) || (Action == SynchronizeChecklist::saDownloadNew);
        QColor Color = New ? QColor(0x2e, 0x9e, 0x2e) : QColor(0x2a, 0x6a, 0xd0);
        QPainterPath Path;
        if (Right)
        {
          Path.moveTo(1, 6); Path.lineTo(8, 6); Path.lineTo(8, 2); Path.lineTo(15, 8);
          Path.lineTo(8, 14); Path.lineTo(8, 10); Path.lineTo(1, 10); Path.closeSubpath();
        }
        else
        {
          Path.moveTo(15, 6); Path.lineTo(8, 6); Path.lineTo(8, 2); Path.lineTo(1, 8);
          Path.lineTo(8, 14); Path.lineTo(8, 10); Path.lineTo(15, 10); Path.closeSubpath();
        }
        Painter.fillPath(Path, Color);
        Painter.setPen(QPen(Color.darker(140), 1));
        Painter.drawPath(Path);
      }
      break;

    case SynchronizeChecklist::saDeleteRemote:
    case SynchronizeChecklist::saDeleteLocal:
      {
        int X = (Action == SynchronizeChecklist::saDeleteRemote) ? 6 : 0;
        Painter.setPen(QPen(QColor(0xd0, 0x20, 0x20), 2.5, Qt::SolidLine, Qt::RoundCap));
        Painter.drawLine(X + 2, 3, X + 9, 12);
        Painter.drawLine(X + 9, 3, X + 2, 12);
      }
      break;

    default:
      break;
  }
  Painter.end();
  QIcon Icon(Pixmap);
  Cache[Action] = Icon;
  return Icon;
}

QString ActionHint(SynchronizeChecklist::ActionType Action)
{
  switch (Action)
  {
    case SynchronizeChecklist::saUploadNew: return QStringLiteral("Upload new local file");
    case SynchronizeChecklist::saDownloadNew: return QStringLiteral("Download new remote file");
    case SynchronizeChecklist::saUploadUpdate: return QStringLiteral("Upload updated local file");
    case SynchronizeChecklist::saDownloadUpdate: return QStringLiteral("Download updated remote file");
    case SynchronizeChecklist::saDeleteRemote: return QStringLiteral("Delete obsolete remote file");
    case SynchronizeChecklist::saDeleteLocal: return QStringLiteral("Delete obsolete local file");
    default: return QString();
  }
}

// MessageDialog with qpNeverAskAgainCheck (TMessageForm: WinSCP button
// order, Help button); returns the answer as a QMessageBox button
QMessageBox::StandardButton MessageWithNeverAsk(QWidget * Parent, QMessageBox::Icon Icon, const QString & MainInstruction,
  const QString & Text, QMessageBox::StandardButtons Buttons, const QString & CheckCaption, bool & Checked,
  const QString & HelpKeyword = QString())
{
  QueryType Type;
  switch (Icon)
  {
    case QMessageBox::Warning: Type = qtWarning; break;
    case QMessageBox::Critical: Type = qtError; break;
    case QMessageBox::Information: Type = qtInformation; break;
    default: Type = qtConfirmation; break;
  }
  unsigned int Answers = 0;
  if (Buttons & QMessageBox::Yes) Answers |= qaYes;
  if (Buttons & QMessageBox::No) Answers |= qaNo;
  if (Buttons & QMessageBox::Cancel) Answers |= qaCancel;
  if (Buttons & QMessageBox::Ok) Answers |= qaOK;
  MessageParams Params;
  Params.Params = qpNeverAskAgainCheck;
  Params.NeverAskAgainTitle = CheckCaption;
  QString Message = QStringLiteral("**%1**").arg(MainInstruction);
  if (!Text.isEmpty())
  {
    Message += QStringLiteral("\n\n") + Text;
  }
  unsigned int Answer = MessageDialog(Parent, Message, Type, Answers, HelpKeyword, &Params);
  Checked = (Answer == qaNeverAskAgain);
  if (Checked)
  {
    // the positive answer of the dialog
    Answer = (Answers & qaYes) ? qaYes : qaOK;
  }
  switch (Answer)
  {
    case qaYes: return QMessageBox::Yes;
    case qaNo: return QMessageBox::No;
    case qaOK: return QMessageBox::Ok;
    default: return QMessageBox::Cancel;
  }
}

QString SelectLocalDirectory(QWidget * Parent, const QString & Directory)
{
  // SELECT_LOCAL_DIRECTORY
  return QFileDialog::getExistingDirectory(Parent, QStringLiteral("Select local directory."), Directory);
}

// THistoryComboBox for directories (history in Configuration\History\<HistoryKey>)
QComboBox * DirectoryCombo(const QString & HistoryKey)
{
  auto * Combo = new QComboBox();
  Combo->setEditable(true);
  Combo->setInsertPolicy(QComboBox::NoInsert);
  Combo->setMinimumContentsLength(40);
  Combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  Combo->lineEdit()->setMaxLength(1000);
  Combo->setProperty("HistoryKey", HistoryKey);
  EditMaskDialog::LoadHistoryCombo(Combo, HistoryKey);
  Combo->setEditText(QString());
  return Combo;
}

// THistoryComboBox::SaveToHistory (on OK/Start)
void SaveDirectoryHistory(QComboBox * Combo)
{
  QString Key = Combo->property("HistoryKey").toString();
  QString Text = Combo->currentText();
  if (!Key.isEmpty() && !Text.isEmpty())
  {
    EditMaskDialog::SaveToHistory(Key, Text);
    EditMaskDialog::LoadHistoryCombo(Combo, Key);
    Combo->setEditText(Text);
  }
}

// Split button (bsSplitButton): the push button runs its default command
// (and can be the default button of the dialog), the arrow next to it runs
// DropDown with the position for the drop-down menu
struct SplitButtonWidgets
{
  QWidget * Container;
  QPushButton * Button;
  QToolButton * Arrow;
};

SplitButtonWidgets SplitButton(const QString & Caption, std::function<void(const QPoint & GlobalPos)> DropDown, QWidget * Parent)
{
  SplitButtonWidgets Result;
  Result.Container = new QWidget(Parent);
  auto * Layout = new QHBoxLayout(Result.Container);
  Layout->setContentsMargins(0, 0, 0, 0);
  Layout->setSpacing(0);
  Result.Button = new QPushButton(Caption, Result.Container);
  Result.Arrow = new QToolButton(Result.Container);
  Result.Arrow->setArrowType(Qt::DownArrow);
  Result.Arrow->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
  Result.Arrow->setFocusPolicy(Qt::NoFocus);
  Result.Arrow->setAccessibleName(Caption);
  QToolButton * Arrow = Result.Arrow;
  QObject::connect(Arrow, &QToolButton::clicked, Arrow, [Arrow, DropDown]()
  {
    DropDown(Arrow->mapToGlobal(QPoint(0, Arrow->height())));
  });
  Layout->addWidget(Result.Button);
  Layout->addWidget(Result.Arrow);
  return Result;
}

// Menu with a bold default item followed by others
QAction * AddDefaultMenuItem(QMenu & Menu, const QString & Caption)
{
  QAction * Action = Menu.addAction(Caption);
  QFont BoldFont = Action->font();
  BoldFont.setBold(true);
  Action->setFont(BoldFont);
  return Action;
}

// OpenFileInExplorer / OpenFolderInExplorer: org.freedesktop.FileManager1
// ShowItems (selects the file), falling back to opening the folder
void ExploreLocalPath(const QString & Path, bool Folder)
{
  QFileInfo Info(Path);
  if (!Folder && Info.exists())
  {
    QStringList Arguments{
      QStringLiteral("--session"), QStringLiteral("--dest=org.freedesktop.FileManager1"), QStringLiteral("--type=method_call"),
      QStringLiteral("/org/freedesktop/FileManager1"), QStringLiteral("org.freedesktop.FileManager1.ShowItems"),
      QStringLiteral("array:string:%1").arg(QString::fromUtf8(QUrl::fromLocalFile(Info.absoluteFilePath()).toEncoded())),
      QStringLiteral("string:")};
    QProcess Process;
    Process.start(QStringLiteral("dbus-send"), Arguments);
    if (Process.waitForFinished(3000) && (Process.exitStatus() == QProcess::NormalExit) && (Process.exitCode() == 0))
    {
      return;
    }
  }
  QString Directory = (Folder || Info.isDir()) ? Path : Info.absolutePath();
  while (!Directory.isEmpty() && !QFileInfo(Directory).isDir() && (Directory != QStringLiteral("/")))
  {
    Directory = QFileInfo(Directory).absolutePath();
  }
  QDesktopServices::openUrl(QUrl::fromLocalFile(Directory));
}

// Minimizes the application (TApplication::Minimize): all top level windows
void MinimizeApplication(QWidget * Widget)
{
  QWidget * Window = Widget->window();
  QWidget * Main = Window->parentWidget() ? Window->parentWidget()->window() : Window;
  Main->showMinimized();
  if (Window != Main)
  {
    Window->showMinimized();
  }
}

// RestoreForm / StoreForm for SynchronizeChecklist\WindowParams
void RestoreWindowParams(QWidget * Form, const QString & WindowParams, QWidget * MainWindow)
{
  QStringList Parts = WindowParams.split(QLatin1Char(';'));
  bool CustomPos = (Parts.value(0).toInt() != 0);
  int Left = Parts.value(1, QStringLiteral("-1")).toInt();
  int Top = Parts.value(2, QStringLiteral("-1")).toInt();
  int Right = Parts.value(3).toInt();
  int Bottom = Parts.value(4).toInt();
  int State = Parts.value(5).toInt();
  int PixelsPerInch = Parts.value(6, QStringLiteral("96")).toInt();
  double Scale = ((PixelsPerInch > 0) && (Form->logicalDpiX() > 0)) ? static_cast<double>(Form->logicalDpiX()) / PixelsPerInch : 1.0;
  auto Dim = [Scale](int Value) { return static_cast<int>(Value * Scale); };
  QScreen * Screen = (MainWindow != nullptr) ? MainWindow->screen() : Form->screen();
  QRect Available = (Screen != nullptr) ? Screen->availableGeometry() : QRect(0, 0, 1920, 1080);
  if (CustomPos || (MainWindow == nullptr))
  {
    if ((Left == -1) && (Top == -1))
    {
      // FormatDefaultWindowParams: size only
      QSize Size(std::min(Dim(Right), Available.width() - 40), std::min(Dim(Bottom), Available.height() - 30));
      if ((Size.width() > 100) && (Size.height() > 100))
      {
        Form->resize(Size);
      }
    }
    else if ((Right > Left) && (Bottom > Top))
    {
      QRect Bounds(QPoint(Dim(Left), Dim(Top)), QPoint(Dim(Right) - 1, Dim(Bottom) - 1));
      Bounds.translate(Available.topLeft());
      if (Available.intersects(Bounds))
      {
        Form->setGeometry(Bounds);
      }
      else
      {
        Form->resize(Bounds.size());
      }
    }
  }
  else
  {
    // BoundsRect = Application->MainForm->BoundsRect
    Form->setGeometry(MainWindow->geometry());
  }
  if (State == 2) // wsMaximized
  {
    Form->setWindowState(Form->windowState() | Qt::WindowMaximized);
  }
}

QString StoreWindowParams(QWidget * Form, QWidget * MainWindow, const QString & PrevWindowParams)
{
  bool CustomPos = (PrevWindowParams.section(QLatin1Char(';'), 0, 0).toInt() != 0);
  if (MainWindow != nullptr)
  {
    CustomPos = (MainWindow->geometry() != Form->geometry());
  }
  QRect Bounds = Form->isMaximized() ? Form->normalGeometry() : Form->geometry();
  QScreen * Screen = Form->screen();
  if (Screen != nullptr)
  {
    Bounds.translate(-Screen->availableGeometry().topLeft());
  }
  return QStringLiteral("%1;%2;%3;%4;%5;%6;%7").arg(CustomPos ? 1 : 0).arg(Bounds.left()).arg(Bounds.top())
    .arg(Bounds.right() + 1).arg(Bounds.bottom() + 1).arg(Form->isMaximized() ? 2 : 0).arg(Form->logicalDpiX());
}

// Clickable label of the "Transfer settings" group (CopyParamLabel)
class CopyParamLabel : public QLabel
{
public:
  std::function<void()> OnClick;
  CopyParamLabel()
  {
    setWordWrap(true);
    setTextFormat(Qt::PlainText);
    setCursor(Qt::PointingHandCursor);
    setMinimumHeight(fontMetrics().height() * 2);
    setAlignment(Qt::AlignLeft | Qt::AlignTop);
  }
protected:
  void mouseReleaseEvent(QMouseEvent * Event) override
  {
    QLabel::mouseReleaseEvent(Event);
    if (OnClick)
    {
      OnClick();
    }
  }
};

// TPathLabel: elides the middle of the path
class PathLabel : public QLabel
{
public:
  PathLabel()
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
  QSize sizeHint() const override { return QSize(300, fontMetrics().height()); }
  QSize minimumSizeHint() const override { return QSize(50, fontMetrics().height()); }
protected:
  void paintEvent(QPaintEvent *) override
  {
    QPainter Painter(this);
    QRect R = contentsRect();
    Painter.setPen(palette().color(QPalette::WindowText));
    Painter.drawText(R, Qt::AlignLeft | Qt::AlignVCenter, fontMetrics().elidedText(FPath, Qt::ElideMiddle, R.width()));
  }
private:
  QString FPath;
};

}

//---------------------------------------------------------------------------
// TGUIConfiguration synchronization settings

SynchronizeUiHooks & SynchronizeHooks()
{
  static SynchronizeUiHooks Hooks;
  return Hooks;
}

SynchronizeConfiguration SynchronizeConfiguration::Load()
{
  SynchronizeConfiguration Result;
  {
    IniStorage Storage(&Configuration::Instance().Ini());
    if (Storage.OpenSubKeyPath(InterfaceKey, false))
    {
      Result.Params = Storage.ReadInteger(QStringLiteral("SynchronizeParams"), Result.Params);
      Result.Options = Storage.ReadInteger(QStringLiteral("SynchronizeOptions"), Result.Options);
      Result.ModeAuto = Storage.ReadInteger(QStringLiteral("SynchronizeModeAuto"), Result.ModeAuto);
      Result.Mode = Storage.ReadInteger(QStringLiteral("SynchronizeMode"), Result.Mode);
      Result.MaxWatchDirectories = Storage.ReadInteger(QStringLiteral("MaxWatchDirectories"), Result.MaxWatchDirectories);
      Result.KeepUpToDateChangeDelay = Storage.ReadInteger(QStringLiteral("KeepUpToDateChangeDelay"), Result.KeepUpToDateChangeDelay);
    }
  }
  {
    IniStorage Storage(&Configuration::Instance().Ini());
    if (Storage.OpenSubKeyPath(ChecklistKey, false))
    {
      Result.ChecklistWindowParams = Storage.ReadString(QStringLiteral("WindowParams"), Result.ChecklistWindowParams);
      Result.ChecklistListParams = Storage.ReadString(QStringLiteral("ListParams"), Result.ChecklistListParams);
    }
  }
  if ((Result.ModeAuto < -1) || (Result.ModeAuto > smBoth))
  {
    Result.ModeAuto = -1;
  }
  return Result;
}

void SynchronizeConfiguration::Save() const
{
  {
    IniStorage Storage(&Configuration::Instance().Ini());
    if (Storage.OpenSubKeyPath(InterfaceKey, true))
    {
      Storage.WriteInteger(QStringLiteral("SynchronizeParams"), Params);
      Storage.WriteInteger(QStringLiteral("SynchronizeOptions"), Options);
      Storage.WriteInteger(QStringLiteral("SynchronizeModeAuto"), ModeAuto);
      Storage.WriteInteger(QStringLiteral("SynchronizeMode"), Mode);
      Storage.WriteInteger(QStringLiteral("MaxWatchDirectories"), MaxWatchDirectories);
      Storage.WriteInteger(QStringLiteral("KeepUpToDateChangeDelay"), KeepUpToDateChangeDelay);
    }
  }
  {
    IniStorage Storage(&Configuration::Instance().Ini());
    if (Storage.OpenSubKeyPath(ChecklistKey, true))
    {
      Storage.WriteString(QStringLiteral("WindowParams"), ChecklistWindowParams);
      Storage.WriteString(QStringLiteral("ListParams"), ChecklistListParams);
    }
  }
  Configuration::Instance().Save();
}

namespace
{

//---------------------------------------------------------------------------
// TFullSynchronizeDialog

const int fsoDisableTimestamp = 0x01;
const int fsoAllowSelectedOnly = 0x04;
const int fsoDisableByChecksum = 0x08;

class FullSynchronizeDialog : public QDialog
{
public:
  FullSynchronizeDialog(int Options, QWidget * Parent);

  void SetMode(SynchronizeMode Value);
  SynchronizeMode Mode() const;
  void SetParams(int Value);
  int Params() const;
  void SetDirectory1(const QString & Value) { FLocalDirectoryEdit->setEditText(Value); }
  QString Directory1() const { return FLocalDirectoryEdit->currentText(); }
  void SetDirectory2(const QString & Value) { FRemoteDirectoryEdit->setEditText(Value); }
  QString Directory2() const { return FRemoteDirectoryEdit->currentText(); }
  void SetCopyParams(const CopyParam & Value) { FCopyParams = Value; UpdateControls(); }
  CopyParam CopyParams() const;
  void SetSaveSettings(bool Value) { FSaveSettingsCheck->setChecked(Value); }
  bool SaveSettings() const { return FSaveSettingsCheck->isChecked(); }
  void SetSaveMode(bool Value) { FSaveMode = Value; }
  bool SaveMode() const { return FSaveMode; }
  // OK drop-down > Start in New Window was chosen
  bool StartInNewWindow() const { return FStartInNewWindow; }

  void accept() override;

private:
  int FOptions;
  int FParams = 0;
  bool FSaveMode = false;
  bool FStartInNewWindow = false;
  SynchronizeMode FOrigMode = smRemote;
  CopyParam FCopyParams;
  QString FPreset;
  bool FUpdating = false;

  QComboBox * FLocalDirectoryEdit;
  QComboBox * FRemoteDirectoryEdit;
  QRadioButton * FSynchronizeBothButton;
  QRadioButton * FSynchronizeRemoteButton;
  QRadioButton * FSynchronizeLocalButton;
  QRadioButton * FSynchronizeFilesButton;
  QRadioButton * FMirrorFilesButton;
  QRadioButton * FSynchronizeTimestampsButton;
  QCheckBox * FSynchronizeDeleteCheck;
  QCheckBox * FSynchronizePreviewChangesCheck;
  QCheckBox * FSynchronizeExistingOnlyCheck;
  QCheckBox * FSynchronizeSelectedOnlyCheck;
  QCheckBox * FSynchronizeByTimeCheck;
  QCheckBox * FSynchronizeBySizeCheck;
  QCheckBox * FSynchronizeByChecksumCheck;
  QCheckBox * FSynchronizeCaseSensitiveCheck;
  QCheckBox * FSaveSettingsCheck;
  CopyParamLabel * FCopyParamLabel;
  QPushButton * FOkButton;

  bool CanSynchronizeTimestamps() const { return (FOptions & fsoDisableTimestamp) == 0; }
  void CopyParamListPopup(const QPoint & GlobalPos);
  int ActualCopyParamAttrs() const;
  void UpdateControls();
  void CopyParamGroupClick();
};

FullSynchronizeDialog::FullSynchronizeDialog(int Options, QWidget * Parent) :
  QDialog(Parent),
  FOptions(Options)
{
  setWindowTitle(QStringLiteral("Synchronize"));
  auto * Layout = new QVBoxLayout(this);

  auto * DirectoriesGroup = new QGroupBox(QStringLiteral("Directories"));
  auto * DirectoriesLayout = new QGridLayout(DirectoriesGroup);
  auto * LocalDirectoryLabel = new QLabel(QStringLiteral("Lo&cal directory:"));
  FLocalDirectoryEdit = DirectoryCombo(LocalDirectoryHistory);
  LocalDirectoryLabel->setBuddy(FLocalDirectoryEdit);
  auto * LocalDirectoryBrowseButton = new QPushButton(QStringLiteral("Bro&wse..."));
  auto * RemoteDirectoryLabel = new QLabel(QStringLiteral("R&emote directory:"));
  FRemoteDirectoryEdit = DirectoryCombo(RemoteDirectoryHistory);
  RemoteDirectoryLabel->setBuddy(FRemoteDirectoryEdit);
  DirectoriesLayout->addWidget(LocalDirectoryLabel, 0, 0, 1, 2);
  DirectoriesLayout->addWidget(FLocalDirectoryEdit, 1, 0);
  DirectoriesLayout->addWidget(LocalDirectoryBrowseButton, 1, 1);
  DirectoriesLayout->addWidget(RemoteDirectoryLabel, 2, 0, 1, 2);
  DirectoriesLayout->addWidget(FRemoteDirectoryEdit, 3, 0, 1, 2);
  Layout->addWidget(DirectoriesGroup);

  auto * Grid = new QGridLayout();
  auto * DirectionGroup = new QGroupBox(QStringLiteral("Direction/Target directory"));
  auto * DirectionLayout = new QHBoxLayout(DirectionGroup);
  FSynchronizeBothButton = new QRadioButton(QStringLiteral("&Both"));
  FSynchronizeRemoteButton = new QRadioButton(QStringLiteral("&Remote"));
  FSynchronizeLocalButton = new QRadioButton(QStringLiteral("&Local"));
  DirectionLayout->addWidget(FSynchronizeBothButton);
  DirectionLayout->addWidget(FSynchronizeRemoteButton);
  DirectionLayout->addWidget(FSynchronizeLocalButton);
  DirectionLayout->addStretch();
  FSynchronizeBothButton->setChecked(true);

  auto * ModeGroup = new QGroupBox(QStringLiteral("Mode"));
  auto * ModeLayout = new QVBoxLayout(ModeGroup);
  FSynchronizeFilesButton = new QRadioButton(QStringLiteral("Synchronize &files"));
  FMirrorFilesButton = new QRadioButton(QStringLiteral("&Mirror files"));
  FSynchronizeTimestampsButton = new QRadioButton(QStringLiteral("Synchronize &timestamps"));
  ModeLayout->addWidget(FSynchronizeFilesButton);
  ModeLayout->addWidget(FMirrorFilesButton);
  ModeLayout->addWidget(FSynchronizeTimestampsButton);
  FSynchronizeFilesButton->setChecked(true);

  auto * OptionsGroup = new QGroupBox(QStringLiteral("Synchronize options"));
  auto * OptionsLayout = new QGridLayout(OptionsGroup);
  FSynchronizeDeleteCheck = new QCheckBox(QStringLiteral("&Delete files"));
  FSynchronizePreviewChangesCheck = new QCheckBox(QStringLiteral("Pre&view changes"));
  FSynchronizeExistingOnlyCheck = new QCheckBox(QStringLiteral("E&xisting files only"));
  FSynchronizeSelectedOnlyCheck = new QCheckBox(QStringLiteral("Selected files o&nly"));
  OptionsLayout->addWidget(FSynchronizeDeleteCheck, 0, 0);
  OptionsLayout->addWidget(FSynchronizePreviewChangesCheck, 0, 1);
  OptionsLayout->addWidget(FSynchronizeExistingOnlyCheck, 1, 0);
  OptionsLayout->addWidget(FSynchronizeSelectedOnlyCheck, 1, 1);

  auto * CompareCriterionsGroup = new QGroupBox(QStringLiteral("Comparison criteria"));
  auto * CompareLayout = new QGridLayout(CompareCriterionsGroup);
  FSynchronizeByTimeCheck = new QCheckBox(QStringLiteral("M&odification time"));
  FSynchronizeBySizeCheck = new QCheckBox(QStringLiteral("File si&ze"));
  FSynchronizeByChecksumCheck = new QCheckBox(QStringLiteral("Checks&um"));
  FSynchronizeCaseSensitiveCheck = new QCheckBox(QStringLiteral("C&ase sensitive"));
  CompareLayout->addWidget(FSynchronizeByTimeCheck, 0, 0);
  CompareLayout->addWidget(FSynchronizeBySizeCheck, 0, 1);
  CompareLayout->addWidget(FSynchronizeByChecksumCheck, 1, 0);
  CompareLayout->addWidget(FSynchronizeCaseSensitiveCheck, 1, 1);

  Grid->addWidget(DirectionGroup, 0, 0);
  Grid->addWidget(ModeGroup, 0, 1, 2, 1);
  Grid->addWidget(OptionsGroup, 1, 0, 2, 1);
  Grid->addWidget(CompareCriterionsGroup, 2, 1);
  Layout->addLayout(Grid);

  FSaveSettingsCheck = new QCheckBox(QStringLiteral("Use &same options next time"));
  Layout->addWidget(FSaveSettingsCheck);

  auto * CopyParamGroup = new QGroupBox(QStringLiteral("Transfer settings"));
  auto * CopyParamLayout = new QVBoxLayout(CopyParamGroup);
  FCopyParamLabel = new CopyParamLabel();
  FCopyParamLabel->OnClick = [this]() { CopyParamGroupClick(); };
  CopyParamLayout->addWidget(FCopyParamLabel);
  Layout->addWidget(CopyParamGroup);
  // CopyParamGroupContextPopup
  CopyParamGroup->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(CopyParamGroup, &QWidget::customContextMenuRequested, this, [this, CopyParamGroup](const QPoint & Pos)
  {
    CopyParamListPopup(CopyParamGroup->mapToGlobal(Pos));
  });

  auto * Buttons = new QHBoxLayout();
  SplitButtonWidgets TransferSettings =
    SplitButton(QStringLiteral("Transfer settin&gs..."), [this](const QPoint & Pos) { CopyParamListPopup(Pos); }, this);
  QPushButton * TransferSettingsButton = TransferSettings.Button;
  TransferSettingsButton->setAutoDefault(false);
  // OkMenu: Start / Start in New Window
  SplitButtonWidgets Ok = SplitButton(QStringLiteral("OK"), [this](const QPoint & Pos)
  {
    QMenu Menu(this);
    QAction * StartAction = AddDefaultMenuItem(Menu, QStringLiteral("&Start"));
    QAction * NewWindowAction = Menu.addAction(QStringLiteral("Start in &New Window"));
    QAction * Chosen = Menu.exec(Pos);
    if ((Chosen == StartAction) || (Chosen == NewWindowAction))
    {
      FStartInNewWindow = (Chosen == NewWindowAction);
      accept();
    }
  }, this);
  FOkButton = Ok.Button;
  FOkButton->setDefault(true);
  Ok.Arrow->setVisible(static_cast<bool>(SynchronizeHooks().FullSynchronizeInNewWindow));
  auto * CancelButton = new QPushButton(QStringLiteral("Cancel"));
  CancelButton->setAutoDefault(false);
  auto * HelpButton = new QPushButton(QStringLiteral("&Help"));
  HelpButton->setAutoDefault(false);
  HelpSystem::InstallFormHelp(this, QStringLiteral("ui_synchronize"), HelpButton);
  Buttons->addWidget(TransferSettings.Container);
  Buttons->addStretch();
  Buttons->addWidget(Ok.Container);
  Buttons->addWidget(CancelButton);
  Buttons->addWidget(HelpButton);
  Layout->addLayout(Buttons);

  connect(LocalDirectoryBrowseButton, &QPushButton::clicked, this, [this]()
  {
    QString Directory = SelectLocalDirectory(this, FLocalDirectoryEdit->currentText());
    if (!Directory.isEmpty())
    {
      FLocalDirectoryEdit->setEditText(Directory);
    }
  });
  connect(TransferSettingsButton, &QPushButton::clicked, this, [this]() { CopyParamGroupClick(); });
  connect(FOkButton, &QPushButton::clicked, this, [this]() { FStartInNewWindow = false; accept(); });
  connect(CancelButton, &QPushButton::clicked, this, &QDialog::reject);
  connect(FLocalDirectoryEdit, &QComboBox::editTextChanged, this, [this]() { UpdateControls(); });
  connect(FRemoteDirectoryEdit, &QComboBox::editTextChanged, this, [this]() { UpdateControls(); });
  for (QAbstractButton * Button : std::initializer_list<QAbstractButton *>{
         FSynchronizeBothButton, FSynchronizeRemoteButton, FSynchronizeLocalButton, FSynchronizeFilesButton,
         FMirrorFilesButton, FSynchronizeTimestampsButton, FSynchronizeDeleteCheck, FSynchronizePreviewChangesCheck,
         FSynchronizeExistingOnlyCheck, FSynchronizeSelectedOnlyCheck, FSynchronizeByTimeCheck, FSynchronizeBySizeCheck,
         FSynchronizeByChecksumCheck, FSynchronizeCaseSensitiveCheck})
  {
    connect(Button, &QAbstractButton::toggled, this, [this]() { UpdateControls(); });
  }

  if (!CanSynchronizeTimestamps() && FSynchronizeTimestampsButton->isChecked())
  {
    FSynchronizeFilesButton->setChecked(true);
  }
  UpdateControls();
}

void FullSynchronizeDialog::UpdateControls()
{
  if (FUpdating)
  {
    return;
  }
  FUpdating = true;
  FSynchronizeTimestampsButton->setEnabled(CanSynchronizeTimestamps());
  if (FSynchronizeTimestampsButton->isChecked())
  {
    FSynchronizeExistingOnlyCheck->setChecked(true);
    FSynchronizeDeleteCheck->setChecked(false);
    FSynchronizeByTimeCheck->setChecked(true);
    FSynchronizeByChecksumCheck->setChecked(false);
  }
  if (FSynchronizeBothButton->isChecked())
  {
    FSynchronizeByTimeCheck->setChecked(true);
    FSynchronizeBySizeCheck->setChecked(false);
    FSynchronizeByChecksumCheck->setChecked(false);
    if (FMirrorFilesButton->isChecked())
    {
      FSynchronizeFilesButton->setChecked(true);
    }
  }
  bool Both = FSynchronizeBothButton->isChecked();
  bool Timestamps = FSynchronizeTimestampsButton->isChecked();
  FMirrorFilesButton->setEnabled(!Both);
  FSynchronizeDeleteCheck->setEnabled(!Both && !Timestamps);
  FSynchronizeExistingOnlyCheck->setEnabled(!Timestamps);
  FSynchronizeByTimeCheck->setEnabled(!Both && !Timestamps);
  FSynchronizeBySizeCheck->setEnabled(!Both);
  FSynchronizeByChecksumCheck->setEnabled(((FOptions & fsoDisableByChecksum) == 0) && !Both && !Timestamps);
  FSynchronizeSelectedOnlyCheck->setEnabled((FOptions & fsoAllowSelectedOnly) != 0);

  FOkButton->setEnabled(!FLocalDirectoryEdit->currentText().isEmpty() && !FRemoteDirectoryEdit->currentText().isEmpty());

  QString InfoStr = FCopyParams.GetInfoStr(QStringLiteral("; "), ActualCopyParamAttrs());
  FCopyParamLabel->setText(InfoStr);
  FCopyParamLabel->setToolTip(InfoStr);

  // SYNCHRONIZE_SAME_SIZE
  FSynchronizeBySizeCheck->setText(Timestamps ? QStringLiteral("Same si&ze only") : QStringLiteral("File si&ze"));
  FUpdating = false;
}

int FullSynchronizeDialog::ActualCopyParamAttrs() const
{
  int Result;
  if (FSynchronizeTimestampsButton->isChecked())
  {
    Result = cpaIncludeMaskOnly;
  }
  else
  {
    switch (Mode())
    {
      case smRemote:
        Result = CopyParam::UsableAttrs(true);
        break;
      case smLocal:
        Result = CopyParam::UsableAttrs(false);
        break;
      default:
        Result = CopyParam::UsableAttrs(true) & CopyParam::UsableAttrs(false);
        break;
    }
  }
  return Result | (FSynchronizeByTimeCheck->isChecked() ? cpaNoPreserveTime : 0) | cpaNoNewerOnly;
}

void FullSynchronizeDialog::CopyParamListPopup(const QPoint & GlobalPos)
{
  // We pass in FCopyParams, although it may not be the exact copy param
  // that will be used (because of PreserveTime). The reason is to
  // display checkbox next to user-selected preset
  CopyParam ACopyParams = FCopyParams;
  if (::CopyParamListPopup(this, GlobalPos, ACopyParams, FPreset, cplCustomizeDefault, ActualCopyParamAttrs()) > 0)
  {
    FCopyParams = ACopyParams;
  }
  UpdateControls();
}

void FullSynchronizeDialog::CopyParamGroupClick()
{
  // PreserveTime is forced for some settings, but avoid hard-setting it until
  // user really confirms it on custom dialog
  CopyParam ACopyParams = CopyParams();
  if (CopyParamsDialog::Execute(ACopyParams, ActualCopyParamAttrs(), this))
  {
    FCopyParams = ACopyParams;
    UpdateControls();
  }
}

void FullSynchronizeDialog::SetMode(SynchronizeMode Value)
{
  FOrigMode = Value;
  switch (Value)
  {
    case smRemote: FSynchronizeRemoteButton->setChecked(true); break;
    case smLocal: FSynchronizeLocalButton->setChecked(true); break;
    default: FSynchronizeBothButton->setChecked(true); break;
  }
}

SynchronizeMode FullSynchronizeDialog::Mode() const
{
  if (FSynchronizeRemoteButton->isChecked())
  {
    return smRemote;
  }
  else if (FSynchronizeLocalButton->isChecked())
  {
    return smLocal;
  }
  return smBoth;
}

void FullSynchronizeDialog::SetParams(int Value)
{
  FParams = Value &
    ~(spDelete | spExistingOnly | spPreviewChanges | spTimestamp | spNotByTime | spBySize |
      spSelectedOnly | spMirror | spCaseSensitive | spByChecksum);
  FSynchronizeDeleteCheck->setChecked((Value & spDelete) != 0);
  FSynchronizeExistingOnlyCheck->setChecked((Value & spExistingOnly) != 0);
  FSynchronizePreviewChangesCheck->setChecked((Value & spPreviewChanges) != 0);
  FSynchronizeSelectedOnlyCheck->setChecked((Value & spSelectedOnly) != 0);
  if (((Value & spTimestamp) != 0) && CanSynchronizeTimestamps())
  {
    FSynchronizeTimestampsButton->setChecked(true);
  }
  else if ((Value & spMirror) != 0)
  {
    FMirrorFilesButton->setChecked(true);
  }
  else
  {
    FSynchronizeFilesButton->setChecked(true);
  }
  FSynchronizeByTimeCheck->setChecked((Value & spNotByTime) == 0);
  FSynchronizeBySizeCheck->setChecked((Value & spBySize) != 0);
  FSynchronizeByChecksumCheck->setChecked((Value & spByChecksum) != 0);
  FSynchronizeCaseSensitiveCheck->setChecked((Value & spCaseSensitive) != 0);
  UpdateControls();
}

int FullSynchronizeDialog::Params() const
{
  return FParams |
    (FSynchronizeDeleteCheck->isChecked() ? spDelete : 0) |
    (FSynchronizeExistingOnlyCheck->isChecked() ? spExistingOnly : 0) |
    (FSynchronizePreviewChangesCheck->isChecked() ? spPreviewChanges : 0) |
    (FSynchronizeSelectedOnlyCheck->isChecked() ? spSelectedOnly : 0) |
    ((FSynchronizeTimestampsButton->isChecked() && CanSynchronizeTimestamps()) ? spTimestamp : 0) |
    (FMirrorFilesButton->isChecked() ? spMirror : 0) |
    (!FSynchronizeByTimeCheck->isChecked() ? spNotByTime : 0) |
    (FSynchronizeBySizeCheck->isChecked() ? spBySize : 0) |
    ((FSynchronizeByChecksumCheck->isEnabled() && FSynchronizeByChecksumCheck->isChecked()) ? spByChecksum : 0) |
    (FSynchronizeCaseSensitiveCheck->isChecked() ? spCaseSensitive : 0);
}

CopyParam FullSynchronizeDialog::CopyParams() const
{
  CopyParam Result = FCopyParams;
  // when synchronizing by time, we force preserving time,
  // otherwise it does not make any sense
  if ((Params() & spNotByTime) == 0)
  {
    Result.PreserveTime = true;
  }
  Result.NewerOnly = false;
  return Result;
}

void FullSynchronizeDialog::accept()
{
  // TFullSynchronizeDialog::FormCloseQuery
  if (SaveSettings() && (FOrigMode != Mode()) && !FSaveMode)
  {
    QMessageBox Box(QMessageBox::Question, windowTitle(),
      QStringLiteral("Do you want to make direction you have selected the default?"),
      QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, this);
    Box.setInformativeText(QStringLiteral("You have overridden pre-selected synchronisation direction. "
      "By default the direction is determined from file panel that was active before you run the synchronise function."));
    Box.exec();
    switch ((Box.clickedButton() != nullptr) ? Box.standardButton(Box.clickedButton()) : QMessageBox::Cancel)
    {
      case QMessageBox::Yes:
        FSaveMode = true;
        break;
      case QMessageBox::Cancel:
        return;
      default:
        break;
    }
  }
  SaveDirectoryHistory(FLocalDirectoryEdit);
  SaveDirectoryHistory(FRemoteDirectoryEdit);
  QDialog::accept();
}

//---------------------------------------------------------------------------
// TSynchronizeProgressForm (comparing) and the synchronization part of
// TProgressForm with TSynchronizeProgress (synchronizing)

class SynchronizeProgressDialog : public QDialog
{
public:
  SynchronizeProgressDialog(Terminal * Term, SynchronizeStatusPtr Status, SynchronizeChecklistPtr Checklist, QWidget * Parent);

  void Start();
  void Finish();
  void ItemProcessed(const SynchronizeChecklist::Item * ChecklistItem);

protected:
  void reject() override;
  void closeEvent(QCloseEvent * Event) override;

private:
  QPointer<Terminal> FTerminal;
  SynchronizeStatusPtr FStatus;
  SynchronizeChecklistPtr FChecklist;
  std::unique_ptr<SynchronizeProgress> FProgress;
  bool FCollect;
  bool FCanceled = false;
  QElapsedTimer FTimer;
  QDateTime FStartTime;
  QTimer FUpdateTimer;
  QTimer FShowTimer;
  PathLabel * FLeftDirectoryLabel;
  PathLabel * FRightDirectoryLabel;
  PathLabel * FFileLabel = nullptr;
  QLabel * FStartTimeLabel;
  QLabel * FTimeElapsedLabel;
  QLabel * FTimeLeftLabel = nullptr;
  QProgressBar * FFileProgress = nullptr;
  QProgressBar * FOperationProgress;
  QPushButton * FCancelButton;

  void UpdateControls();
  void CancelOperation();
};

SynchronizeProgressDialog::SynchronizeProgressDialog(Terminal * Term, SynchronizeStatusPtr Status,
  SynchronizeChecklistPtr Checklist, QWidget * Parent) :
  QDialog(Parent),
  FTerminal(Term),
  FStatus(std::move(Status)),
  FChecklist(std::move(Checklist)),
  FCollect(!FChecklist)
{
  setWindowModality(Qt::WindowModal);
  if (FChecklist)
  {
    FProgress = std::make_unique<SynchronizeProgress>(FChecklist);
  }
  auto * Layout = new QVBoxLayout(this);
  auto * Grid = new QGridLayout();
  int Row = 0;
  Grid->addWidget(new QLabel(QStringLiteral("Local:")), Row, 0);
  FLeftDirectoryLabel = new PathLabel();
  Grid->addWidget(FLeftDirectoryLabel, Row++, 1, 1, 3);
  Grid->addWidget(new QLabel(QStringLiteral("Remote:")), Row, 0);
  FRightDirectoryLabel = new PathLabel();
  Grid->addWidget(FRightDirectoryLabel, Row++, 1, 1, 3);
  if (!FCollect)
  {
    Grid->addWidget(new QLabel(QStringLiteral("File:")), Row, 0);
    FFileLabel = new PathLabel();
    Grid->addWidget(FFileLabel, Row++, 1, 1, 3);
    FFileProgress = new QProgressBar();
    FFileProgress->setRange(0, 100);
    FFileProgress->setTextVisible(true);
    Grid->addWidget(FFileProgress, Row++, 0, 1, 4);
  }
  Grid->addWidget(new QLabel(QStringLiteral("Start time:")), Row, 0);
  FStartTimeLabel = new QLabel(QStringLiteral("00:00:00"));
  Grid->addWidget(FStartTimeLabel, Row, 1);
  Grid->addWidget(new QLabel(QStringLiteral("Time elapsed:")), Row, 2);
  FTimeElapsedLabel = new QLabel(QStringLiteral("00:00:00"));
  Grid->addWidget(FTimeElapsedLabel, Row++, 3);
  if (!FCollect)
  {
    // TimeLeftLabel (estimated from the overall progress)
    Grid->addWidget(new QLabel(QStringLiteral("Time left:")), Row, 2);
    FTimeLeftLabel = new QLabel(QStringLiteral("Calculating"));
    Grid->addWidget(FTimeLeftLabel, Row++, 3);
  }
  Grid->setColumnStretch(1, 1);
  Grid->setColumnStretch(3, 1);
  Layout->addLayout(Grid);
  FOperationProgress = new QProgressBar();
  if (FCollect)
  {
    FOperationProgress->setRange(0, 0); // pbstMarquee: number of files unknown
  }
  else
  {
    FOperationProgress->setRange(0, 100);
  }
  Layout->addWidget(FOperationProgress);
  auto * Buttons = new QHBoxLayout();
  Buttons->addStretch();
  FCancelButton = new QPushButton(QStringLiteral("Cancel"));
  FCancelButton->setAutoDefault(false);
  // MinimizeItem (Alt+M)
  auto * MinimizeButton = new QPushButton(QStringLiteral("&Minimize"));
  MinimizeButton->setAutoDefault(false);
  MinimizeButton->setShortcut(QKeySequence(Qt::ALT | Qt::Key_M));
  Buttons->addWidget(FCancelButton);
  Buttons->addWidget(MinimizeButton);
  Layout->addLayout(Buttons);
  connect(FCancelButton, &QPushButton::clicked, this, [this]() { CancelOperation(); });
  connect(MinimizeButton, &QPushButton::clicked, this, [this]() { MinimizeApplication(this); });
  resize(500, sizeHint().height());

  FUpdateTimer.setInterval(200);
  connect(&FUpdateTimer, &QTimer::timeout, this, [this]() { UpdateControls(); });
  FShowTimer.setSingleShot(true);
  connect(&FShowTimer, &QTimer::timeout, this, [this]() { show(); });
  UpdateControls();
}

void SynchronizeProgressDialog::Start()
{
  FTimer.start();
  FStartTime = QDateTime::currentDateTime();
  FStartTimeLabel->setText(FStartTime.time().toString(QStringLiteral("HH:mm:ss")));
  FUpdateTimer.start();
  // do not flash the window for quick operations
  FShowTimer.start(300);
  UpdateControls();
}

void SynchronizeProgressDialog::Finish()
{
  FUpdateTimer.stop();
  FShowTimer.stop();
  hide();
}

void SynchronizeProgressDialog::ItemProcessed(const SynchronizeChecklist::Item * ChecklistItem)
{
  if (FProgress)
  {
    FProgress->ItemProcessed(ChecklistItem);
  }
}

void SynchronizeProgressDialog::UpdateControls()
{
  SynchronizeStatus::Snapshot S = FStatus ? FStatus->Get() : SynchronizeStatus::Snapshot();
  FLeftDirectoryLabel->SetPath(S.LocalDirectory);
  FRightDirectoryLabel->SetPath(S.RemoteDirectory);
  QString Caption = FCollect ? QStringLiteral("Comparing") : QStringLiteral("Synchronizing");
  if (!FCollect && !FTerminal.isNull())
  {
    FileOperationProgress::Snapshot P = FTerminal->Progress().Get();
    FFileLabel->SetPath(P.FileName);
    FFileProgress->setValue((P.FileSize > 0) ? static_cast<int>(std::min<qint64>(100, P.FileTransferred * 100 / P.FileSize)) : 0);
    bool Transfer = (P.Op == FileOperationProgress::Copy) || (P.Op == FileOperationProgress::Move);
    int Progress = FProgress ? FProgress->Progress((P.InProgress && Transfer) ? P.TotalTransferred : 0) : 0;
    FOperationProgress->setValue(Progress);
    Caption = QStringLiteral("%1% %2").arg(Progress).arg(Caption);
    // PROGRESS_TIME_LEFT_CALCULATING until there is some progress
    qint64 Elapsed = FTimer.isValid() ? FTimer.elapsed() : 0;
    if ((Progress > 0) && (Elapsed >= 1000))
    {
      FTimeLeftLabel->setText(FormatDateTimeSpan(Elapsed * (100 - Progress) / Progress));
    }
    else
    {
      FTimeLeftLabel->setText(QStringLiteral("Calculating"));
    }
  }
  setWindowTitle(Caption);
  FTimeElapsedLabel->setText(FormatDateTimeSpan(FTimer.isValid() ? FTimer.elapsed() : 0));
  FCancelButton->setEnabled(!FCanceled);
}

void SynchronizeProgressDialog::CancelOperation()
{
  FCanceled = true;
  if (FStatus)
  {
    FStatus->Cancel();
  }
  if (!FTerminal.isNull())
  {
    FTerminal->Progress().Cancel();
  }
  UpdateControls();
}

void SynchronizeProgressDialog::reject()
{
  // Esc = Cancel; the window closes when the operation finishes
  CancelOperation();
}

void SynchronizeProgressDialog::closeEvent(QCloseEvent * Event)
{
  CancelOperation();
  Event->ignore();
}

//---------------------------------------------------------------------------
// TSynchronizeChecklistDialog

const int ColName = 0;
const int ColLocalDir = 1;
const int ColLocalSize = 2;
const int ColLocalChanged = 3;
const int ColAction = 4;
const int ColRemoteDir = 5;
const int ColRemoteSize = 6;
const int ColRemoteChanged = 7;
const int SortRole = Qt::UserRole + 1;
const int ItemRole = Qt::UserRole + 2;

class ChecklistTreeItem : public QTreeWidgetItem
{
public:
  using QTreeWidgetItem::QTreeWidgetItem;
  bool operator<(const QTreeWidgetItem & Other) const override
  {
    int Column = treeWidget() ? treeWidget()->sortColumn() : 0;
    QVariant A = data(Column, SortRole);
    QVariant B = Other.data(Column, SortRole);
    if (A.isValid() && B.isValid() && (A.typeId() == B.typeId()) && (A.typeId() != QMetaType::QString))
    {
      return QVariant::compare(A, B) == QPartialOrdering::Less;
    }
    return text(Column).compare(Other.text(Column), Qt::CaseInsensitive) < 0;
  }
};

// Operations the checklist runs through the synchronization run (the
// TSynchronizeChecklistDialog events of TCustomScpExplorerForm)
struct ChecklistCallbacks
{
  // OnSynchronize (synchronizes inside the checklist): ProcessedItem is
  // called for every finished item, Done when everything is over
  std::function<void(SynchronizeProcessedItemEvent ProcessedItem, std::function<void()> Done)> Synchronize;
  // OnQueueSynchronize (Start in Background); unset: not offered
  std::function<void()> QueueSynchronize;
  // OnSynchronizeChecklistCalculateSize
  std::function<void(const std::vector<const SynchronizeChecklist::Item *> & Items, std::function<void()> Done)> CalculateSize;
  // OnSynchronizeMove
  std::function<void(bool Remote, const QStringList & FileList, const QString & NewFileName, bool TargetIsDirectory,
    std::function<void(bool Ok)> Done)> Move;
  // OnSynchronizeExplore for the remote side; unset: not offered
  std::function<void(const QString & RemoteDirectory, const QString & FileName)> ExploreRemote;
};

class SynchronizeChecklistDialog : public QDialog
{
public:
  SynchronizeChecklistDialog(SynchronizeChecklistPtr Checklist, SynchronizeMode Mode, int Params,
    const QString & Directory1, const QString & Directory2, const ChecklistCallbacks & Callbacks, QWidget * Parent);

  bool Queued() const { return FQueued; }
  bool Synchronizing() const { return FSynchronizing; }

  // Actions for tests and the context menu
  QAction * CalculateSizeAction() const { return FCalculateSizeAction; }
  QAction * CalculateSizeAllAction() const { return FCalculateSizeAllAction; }
  QAction * MoveAction() const { return FMoveAction; }
  QAction * FindMoveCandidateAction() const { return FFindMoveCandidateAction; }

  void accept() override;
  void reject() override;

protected:
  void closeEvent(QCloseEvent * Event) override;

private:
  SynchronizeChecklistPtr FChecklist;
  SynchronizeMode FMode;
  int FParams;
  QString FDirectory1;
  QString FDirectory2;
  ChecklistCallbacks FCallbacks;
  QWidget * FMainWindow;
  bool FSynchronizing = false;
  bool FBusy = false;
  bool FQueued = false;
  QTreeWidget * FListView;
  QPushButton * FOkButton;
  QPushButton * FCancelButton;
  QAction * FCheckAction;
  QAction * FUncheckAction;
  QAction * FCheckAllAction;
  QAction * FUncheckAllAction;
  QAction * FCheckDirectoryAction;
  QAction * FUncheckDirectoryAction;
  QAction * FReverseAction;
  QAction * FMoveAction;
  QAction * FCalculateSizeAction;
  QAction * FCalculateSizeAllAction;
  QAction * FFindMoveCandidateAction;
  QAction * FExploreLocalAction;
  QAction * FExploreRemoteAction;
  QAction * FLocalPathToClipboardAction;
  QAction * FRemotePathToClipboardAction;
  QAction * FSelectAllAction;
  QToolButton * FPanels[1 + SynchronizeChecklist::ActionCount];
  std::map<const SynchronizeChecklist::Item *, SynchronizeChecklist::ActionType> FActions;
  std::map<const SynchronizeChecklist::Item *, QTreeWidgetItem *> FChecklistToListViewMap;
  int FTotals[1 + SynchronizeChecklist::ActionCount];
  int FChecked[1 + SynchronizeChecklist::ActionCount];
  qint64 FCheckedSize[1 + SynchronizeChecklist::ActionCount];
  int FDirectories = 0;
  bool FMass = false;

  const SynchronizeChecklist::Item * ChecklistItem(QTreeWidgetItem * Item) const
  {
    return reinterpret_cast<const SynchronizeChecklist::Item *>(Item->data(ColName, ItemRole).value<quintptr>());
  }
  SynchronizeChecklist::ActionType & ItemAction(const SynchronizeChecklist::Item * AItem) { return FActions[AItem]; }
  QString RelativeDirectory(const QString & Root, const QString & Directory) const;
  void LoadItem(QTreeWidgetItem * Item);
  void LoadList();
  void CountTotals();
  void UpdateCaption();
  void UpdateControls();
  void UpdateStatusBar();
  bool ActionPossible(SynchronizeChecklist::ActionType Action) const;
  void CheckSelected(bool Check);
  void CheckAll(bool Check);
  void CheckDirectory(bool Check);
  void Reverse();
  void SelectAll(int Action);
  void DoSynchronize(bool Queue);
  void ProcessedItem(const SynchronizeChecklist::Item * AChecklistItem);
  void CalculateSize(bool All);
  void DeleteItem(const SynchronizeChecklist::Item * AChecklistItem);
  void Move();
  void FindMoveCandidate();
  void DoExplore(bool Local);
  void PathToClipboard(bool Local);
  void LoadListParams(const QString & ListParams);
  QString StoreListParams() const;
  void SaveWindowParams();
};

// TMoveActionData / TMoveData of SynchronizeChecklist.cpp
struct MoveActionData
{
  SynchronizeChecklist::ActionType Action = SynchronizeChecklist::saNone;
  bool AllItemsFiles = true;
  bool AllItemsDirectories = true;
  int ItemsCount = 0;
  QStringList FileList;
  std::vector<const SynchronizeChecklist::Item *> ChecklistItems;

  bool Collect(const SynchronizeChecklist::Item * AChecklistItem, SynchronizeChecklist::ActionType AAction, bool CollectFileList)
  {
    bool Result = (ItemsCount == 0) || (Action == AAction);
    if (Result)
    {
      Action = AAction;
      if (AChecklistItem->IsDirectory)
      {
        AllItemsFiles = false;
      }
      else
      {
        AllItemsDirectories = false;
      }
      ItemsCount++;
      if (CollectFileList)
      {
        if (Action == SynchronizeChecklist::saDeleteRemote)
        {
          FileList.append(AChecklistItem->GetRemotePath());
        }
        else if (Action == SynchronizeChecklist::saDeleteLocal)
        {
          FileList.append(AChecklistItem->GetLocalPath());
        }
        ChecklistItems.push_back(AChecklistItem);
      }
    }
    return Result;
  }

  bool IsOnlyDirectoryTransferAction() const
  {
    return SynchronizeChecklist::IsTransferNewAction(Action) && (ItemsCount == 1) && AllItemsDirectories;
  }
};

struct MoveData
{
  MoveActionData FirstAction;
  MoveActionData SecondAction;
  bool ThreeActions = false;
  bool CollectFileList;

  explicit MoveData(bool ACollectFileList) : CollectFileList(ACollectFileList) {}

  void Collect(const SynchronizeChecklist::Item * AChecklistItem, SynchronizeChecklist::ActionType Action)
  {
    if (!FirstAction.Collect(AChecklistItem, Action, CollectFileList) &&
        !SecondAction.Collect(AChecklistItem, Action, CollectFileList))
    {
      ThreeActions = true;
    }
  }
};

SynchronizeChecklistDialog::SynchronizeChecklistDialog(SynchronizeChecklistPtr Checklist, SynchronizeMode Mode, int Params,
  const QString & Directory1, const QString & Directory2, const ChecklistCallbacks & Callbacks, QWidget * Parent) :
  QDialog(Parent),
  FChecklist(std::move(Checklist)),
  FMode(Mode),
  FParams(Params),
  FDirectory1(QDir::cleanPath(QDir(Directory1).absolutePath())),
  FDirectory2(Directory2),
  FCallbacks(Callbacks),
  FMainWindow((Parent != nullptr) ? Parent->window() : nullptr)
{
  while ((FDirectory2.size() > 1) && FDirectory2.endsWith(QLatin1Char('/')))
  {
    FDirectory2.chop(1);
  }
  setWindowFlags(windowFlags() | Qt::WindowMaximizeButtonHint | Qt::WindowMinimizeButtonHint);

  auto * Layout = new QVBoxLayout(this);
  auto * Top = new QHBoxLayout();
  FListView = new QTreeWidget();
  FListView->setRootIsDecorated(false);
  FListView->setUniformRowHeights(true);
  FListView->setSelectionMode(QAbstractItemView::ExtendedSelection);
  FListView->setAllColumnsShowFocus(true);
  FListView->setHeaderLabels({QStringLiteral("Name"), QStringLiteral("Local directory"), QStringLiteral("Size"),
    QStringLiteral("Date modified"), QString(), QStringLiteral("Remote directory"), QStringLiteral("Size"),
    QStringLiteral("Date modified")});
  FListView->headerItem()->setTextAlignment(ColLocalSize, Qt::AlignRight | Qt::AlignVCenter);
  FListView->headerItem()->setTextAlignment(ColRemoteSize, Qt::AlignRight | Qt::AlignVCenter);
  FListView->header()->setSectionResizeMode(ColAction, QHeaderView::Fixed);
  FListView->header()->setStretchLastSection(false);
  FListView->setContextMenuPolicy(Qt::CustomContextMenu);
  FListView->setSortingEnabled(false);
  FListView->header()->setSectionsClickable(true);
  FListView->header()->setSortIndicatorShown(false);
  Top->addWidget(FListView, 1);

  auto MakeAction = [this](const QString & Caption, const QKeySequence & Shortcut = QKeySequence())
  {
    auto * Action = new QAction(Caption, this);
    if (!Shortcut.isEmpty())
    {
      Action->setShortcut(Shortcut);
      Action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
      addAction(Action);
    }
    return Action;
  };
  FCheckAllAction = MakeAction(QStringLiteral("Check &All"));
  FUncheckAllAction = MakeAction(QStringLiteral("Uncheck A&ll"));
  FCheckAction = MakeAction(QStringLiteral("&Check"));
  FUncheckAction = MakeAction(QStringLiteral("&Uncheck"));
  FCheckDirectoryAction = MakeAction(QStringLiteral("Check All Actions in &This Directory"));
  FUncheckDirectoryAction = MakeAction(QStringLiteral("Uncheck All Actions in This &Directory"));
  FReverseAction = MakeAction(QStringLiteral("&Reverse"));
  // ShortCut = 117 (F6)
  FMoveAction = MakeAction(QStringLiteral("&Move"), QKeySequence(Qt::Key_F6));
  // ShortCut = 40973 (Shift+Alt+Enter) / 57357 (Shift+Ctrl+Alt+Enter)
  FCalculateSizeAction = MakeAction(QStringLiteral("C&alculate"), QKeySequence(Qt::SHIFT | Qt::ALT | Qt::Key_Return));
  FCalculateSizeAllAction = MakeAction(QStringLiteral("Calc&ulate All"), QKeySequence(Qt::SHIFT | Qt::CTRL | Qt::ALT | Qt::Key_Return));
  // ShortCut = 49269 (Ctrl+Alt+F6)
  FFindMoveCandidateAction = MakeAction(QStringLiteral("&Find Move Candidate"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_F6));
  FExploreLocalAction = MakeAction(QStringLiteral("&Explore"));
  FExploreRemoteAction = MakeAction(QStringLiteral("&Explore"));
  // ShortCut = 16603 (Ctrl+[) / 16605 (Ctrl+])
  FLocalPathToClipboardAction = MakeAction(QStringLiteral("Copy &Path to Clipboard"), QKeySequence(Qt::CTRL | Qt::Key_BracketLeft));
  FRemotePathToClipboardAction = MakeAction(QStringLiteral("Copy &Path to Clipboard"), QKeySequence(Qt::CTRL | Qt::Key_BracketRight));
  FSelectAllAction = MakeAction(QStringLiteral("Select &All"), QKeySequence::SelectAll);

  auto * Panel = new QVBoxLayout();
  // OkPopupMenu: Start / Start in Background
  SplitButtonWidgets Ok = SplitButton(QStringLiteral("OK"), [this](const QPoint & Pos)
  {
    QMenu Menu(this);
    QAction * StartAction = AddDefaultMenuItem(Menu, QStringLiteral("&Start"));
    StartAction->setEnabled(FOkButton->isEnabled());
    QAction * QueueAction = Menu.addAction(QStringLiteral("Start in &Background"));
    QueueAction->setEnabled(FOkButton->isEnabled());
    QAction * Chosen = Menu.exec(Pos);
    if ((Chosen == StartAction) || (Chosen == QueueAction))
    {
      DoSynchronize(Chosen == QueueAction);
    }
  }, this);
  FOkButton = Ok.Button;
  FOkButton->setDefault(true);
  Ok.Arrow->setVisible(static_cast<bool>(FCallbacks.QueueSynchronize) && ((FParams & spTimestamp) == 0));
  FCancelButton = new QPushButton(QStringLiteral("Cancel"));
  FCancelButton->setAutoDefault(false);
  auto ActionButton = [](QAction * Action)
  {
    auto * Button = new QPushButton(Action->text());
    Button->setAutoDefault(false);
    connect(Button, &QPushButton::clicked, Action, &QAction::trigger);
    connect(Action, &QAction::changed, Button, [Button, Action]() { Button->setEnabled(Action->isEnabled()); });
    return Button;
  };
  auto * ToolsButton = new QPushButton(QStringLiteral("&Tools"));
  ToolsButton->setAutoDefault(false);
  auto * ToolsMenu = new QMenu(ToolsButton);
  ToolsMenu->addAction(FCalculateSizeAction);
  ToolsMenu->addAction(FCalculateSizeAllAction);
  ToolsMenu->addSeparator();
  ToolsMenu->addAction(FFindMoveCandidateAction);
  ToolsButton->setMenu(ToolsMenu);
  Panel->addWidget(Ok.Container);
  Panel->addWidget(FCancelButton);
  Panel->addSpacing(12);
  Panel->addWidget(ActionButton(FCheckAllAction));
  Panel->addWidget(ActionButton(FUncheckAllAction));
  Panel->addWidget(ActionButton(FCheckAction));
  Panel->addWidget(ActionButton(FUncheckAction));
  Panel->addSpacing(12);
  Panel->addWidget(ActionButton(FReverseAction));
  Panel->addWidget(ActionButton(FMoveAction));
  Panel->addWidget(ToolsButton);
  Panel->addStretch();
  auto * HelpButton = new QPushButton(QStringLiteral("&Help"));
  HelpButton->setAutoDefault(false);
  HelpSystem::InstallFormHelp(this, QStringLiteral("ui_synchronize_checklist"), HelpButton);
  Panel->addWidget(HelpButton);
  Top->addLayout(Panel);
  Layout->addLayout(Top, 1);

  auto * StatusBar = new QStatusBar();
  StatusBar->setSizeGripEnabled(true);
  static const char * const PanelNames[] = {
    "Total synchronization actions", "New left files", "New right files", "Updated left files",
    "Updated right files", "Obsolete right files", "Obsolete left files" };
  for (int Index = 0; Index <= SynchronizeChecklist::ActionCount; Index++)
  {
    auto * Button = new QToolButton();
    Button->setAutoRaise(true);
    Button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    if (Index > 0)
    {
      Button->setIcon(ActionIcon(static_cast<SynchronizeChecklist::ActionType>(Index)));
    }
    Button->setToolTip(QStringLiteral("%1\nClick to select all actions of this type").arg(QString::fromLatin1(PanelNames[Index])));
    connect(Button, &QToolButton::clicked, this, [this, Index]() { SelectAll(Index); });
    StatusBar->addWidget(Button);
    FPanels[Index] = Button;
  }
  Layout->addWidget(StatusBar);
  Layout->setContentsMargins(6, 6, 6, 0);

  connect(FOkButton, &QPushButton::clicked, this, [this]() { DoSynchronize(false); });
  connect(FCancelButton, &QPushButton::clicked, this, &QDialog::reject);
  connect(FCheckAllAction, &QAction::triggered, this, [this]() { CheckAll(true); });
  connect(FUncheckAllAction, &QAction::triggered, this, [this]() { CheckAll(false); });
  connect(FCheckAction, &QAction::triggered, this, [this]() { CheckSelected(true); });
  connect(FUncheckAction, &QAction::triggered, this, [this]() { CheckSelected(false); });
  connect(FCheckDirectoryAction, &QAction::triggered, this, [this]() { CheckDirectory(true); });
  connect(FUncheckDirectoryAction, &QAction::triggered, this, [this]() { CheckDirectory(false); });
  connect(FReverseAction, &QAction::triggered, this, [this]() { Reverse(); });
  connect(FMoveAction, &QAction::triggered, this, [this]() { Move(); });
  connect(FCalculateSizeAction, &QAction::triggered, this, [this]() { CalculateSize(false); });
  connect(FCalculateSizeAllAction, &QAction::triggered, this, [this]() { CalculateSize(true); });
  connect(FFindMoveCandidateAction, &QAction::triggered, this, [this]() { FindMoveCandidate(); });
  connect(FExploreLocalAction, &QAction::triggered, this, [this]() { DoExplore(true); });
  connect(FExploreRemoteAction, &QAction::triggered, this, [this]() { DoExplore(false); });
  connect(FLocalPathToClipboardAction, &QAction::triggered, this, [this]() { PathToClipboard(true); });
  connect(FRemotePathToClipboardAction, &QAction::triggered, this, [this]() { PathToClipboard(false); });
  connect(FSelectAllAction, &QAction::triggered, this, [this]() { SelectAll(0); });
  connect(FListView, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *, int Column)
  {
    if (!FMass && (Column == ColName))
    {
      CountTotals();
      UpdateControls();
    }
  });
  connect(FListView, &QTreeWidget::itemSelectionChanged, this, [this]() { UpdateControls(); });
  connect(FListView, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *, int Column)
  {
    // click on the action column reverses (not when selecting)
    if ((Column == ColAction) && !FSynchronizing && !FBusy &&
        ((QApplication::keyboardModifiers() & (Qt::ShiftModifier | Qt::ControlModifier)) == 0))
    {
      Reverse();
    }
  });
  connect(FListView->header(), &QHeaderView::sectionClicked, this, [this](int Section)
  {
    if (Section == ColAction)
    {
      return;
    }
    Qt::SortOrder Order = Qt::AscendingOrder;
    if ((FListView->sortColumn() == Section) && FListView->header()->isSortIndicatorShown() &&
        (FListView->header()->sortIndicatorOrder() == Qt::AscendingOrder))
    {
      Order = Qt::DescendingOrder;
    }
    FListView->header()->setSortIndicatorShown(true);
    FListView->sortItems(Section, Order);
  });
  connect(FListView, &QWidget::customContextMenuRequested, this, [this](const QPoint & Pos)
  {
    // ListViewPopupMenu
    QMenu Menu(this);
    Menu.addAction(FCheckAction);
    Menu.addAction(FUncheckAction);
    Menu.addSeparator();
    Menu.addAction(FCheckDirectoryAction);
    Menu.addAction(FUncheckDirectoryAction);
    Menu.addSeparator();
    Menu.addAction(FReverseAction);
    Menu.addAction(FMoveAction);
    QMenu * CalculateMenu = Menu.addMenu(QStringLiteral("C&alculate"));
    CalculateMenu->addAction(FCalculateSizeAction);
    CalculateMenu->addAction(FCalculateSizeAllAction);
    Menu.addSeparator();
    QMenu * LeftMenu = Menu.addMenu(QStringLiteral("&Left Directory"));
    LeftMenu->addAction(FExploreLocalAction);
    LeftMenu->addAction(FLocalPathToClipboardAction);
    QMenu * RightMenu = Menu.addMenu(QStringLiteral("Rig&ht Directory"));
    if (FCallbacks.ExploreRemote)
    {
      RightMenu->addAction(FExploreRemoteAction);
    }
    RightMenu->addAction(FRemotePathToClipboardAction);
    Menu.addSeparator();
    Menu.addAction(FSelectAllAction);
    Menu.exec(FListView->viewport()->mapToGlobal(Pos));
  });

  SynchronizeConfiguration Config = SynchronizeConfiguration::Load();
  LoadListParams(Config.ChecklistListParams);
  LoadList();
  // GetWindowParams / FormShow
  RestoreWindowParams(this, Config.ChecklistWindowParams, FMainWindow);
  if ((FMainWindow == nullptr) && (width() < 200))
  {
    resize(1000, 550);
  }
}

QString SynchronizeChecklistDialog::RelativeDirectory(const QString & Root, const QString & Directory) const
{
  if (Directory.compare(Root, Qt::CaseInsensitive) == 0)
  {
    return QStringLiteral("./");
  }
  QString Prefix = Root.endsWith(QLatin1Char('/')) ? Root : Root + QLatin1Char('/');
  if (Directory.startsWith(Prefix, Qt::CaseInsensitive))
  {
    return QStringLiteral("./") + Directory.mid(Prefix.size()) + QLatin1Char('/');
  }
  return Directory;
}

void SynchronizeChecklistDialog::LoadItem(QTreeWidgetItem * Item)
{
  const SynchronizeChecklist::Item * CI = ChecklistItem(Item);
  SynchronizeChecklist::ActionType Action = ItemAction(CI);
  QString Name = CI->GetFileName();
  if (CI->IsDirectory)
  {
    Name += QLatin1Char('/');
  }
  Item->setText(ColName, Name);
  Item->setData(ColName, SortRole, Name);
  static const QIcon DirectoryIcon = QApplication::style()->standardIcon(QStyle::SP_DirIcon);
  static const QIcon FileIcon = QApplication::style()->standardIcon(QStyle::SP_FileIcon);
  Item->setIcon(ColName, CI->IsDirectory ? DirectoryIcon : FileIcon);

  auto SetSide = [&](int DirColumn, int SizeColumn, int ChangedColumn, bool Present, bool Info, const QString & Root,
    const SynchronizeChecklist::FileInfo & FileInfo)
  {
    Item->setText(DirColumn, Present ? RelativeDirectory(Root, FileInfo.Directory) : QString());
    bool ShowInfo = Present && Info;
    QString Size;
    // directory sizes are known after Calculate
    if (ShowInfo && CI->HasSize())
    {
      Size = FileListModel::FormatSizeKB(FileInfo.Size);
    }
    Item->setText(SizeColumn, Size);
    Item->setData(SizeColumn, SortRole, (ShowInfo && CI->HasSize()) ? FileInfo.Size : -1);
    Item->setTextAlignment(SizeColumn, Qt::AlignRight | Qt::AlignVCenter);
    Item->setText(ChangedColumn, (ShowInfo && !CI->IsDirectory) ? FormatModification(FileInfo.Modification) : QString());
    Item->setData(ChangedColumn, SortRole, ShowInfo ? FileInfo.Modification : QDateTime());
  };
  SetSide(ColLocalDir, ColLocalSize, ColLocalChanged, (Action != SynchronizeChecklist::saDeleteRemote),
    (Action != SynchronizeChecklist::saDownloadNew), FDirectory1, CI->Info1);
  SetSide(ColRemoteDir, ColRemoteSize, ColRemoteChanged, (Action != SynchronizeChecklist::saDeleteLocal),
    (Action != SynchronizeChecklist::saUploadNew), FDirectory2, CI->Info2);
  Item->setIcon(ColAction, ActionIcon(Action));
  Item->setData(ColAction, SortRole, static_cast<int>(Action));
  Item->setToolTip(ColAction, QStringLiteral("%1\nClick to reverse").arg(ActionHint(Action)));
}

void SynchronizeChecklistDialog::LoadList()
{
  FMass = true;
  FListView->clear();
  FChecklistToListViewMap.clear();
  FDirectories = 0;
  QList<QTreeWidgetItem *> Items;
  for (int Index = 0; Index < FChecklist->Count(); Index++)
  {
    const SynchronizeChecklist::Item * CI = FChecklist->GetItem(Index);
    FActions[CI] = CI->Action;
    auto * Item = new ChecklistTreeItem();
    Item->setFlags(Item->flags() | Qt::ItemIsUserCheckable);
    Item->setData(ColName, ItemRole, QVariant::fromValue(reinterpret_cast<quintptr>(CI)));
    Item->setCheckState(ColName, CI->Checked ? Qt::Checked : Qt::Unchecked);
    LoadItem(Item);
    Items.append(Item);
    FChecklistToListViewMap[CI] = Item;
    if (CI->IsDirectory)
    {
      FDirectories++;
    }
  }
  FListView->addTopLevelItems(Items);
  if (FListView->header()->isSortIndicatorShown())
  {
    FListView->sortItems(FListView->sortColumn(), FListView->header()->sortIndicatorOrder());
  }
  FMass = false;
  CountTotals();
  UpdateControls();
}

void SynchronizeChecklistDialog::CountTotals()
{
  std::fill(std::begin(FTotals), std::end(FTotals), 0);
  std::fill(std::begin(FChecked), std::end(FChecked), 0);
  std::fill(std::begin(FCheckedSize), std::end(FCheckedSize), 0);
  for (int Index = 0; Index < FListView->topLevelItemCount(); Index++)
  {
    QTreeWidgetItem * Item = FListView->topLevelItem(Index);
    const SynchronizeChecklist::Item * CI = ChecklistItem(Item);
    SynchronizeChecklist::ActionType Action = ItemAction(CI);
    FTotals[0]++;
    FTotals[Action]++;
    if (Item->checkState(ColName) == Qt::Checked)
    {
      qint64 Size = CI->HasSize() ? CI->GetSize(Action) : 0;
      FChecked[0]++;
      FChecked[Action]++;
      FCheckedSize[0] += Size;
      FCheckedSize[Action] += Size;
    }
  }
}

bool SynchronizeChecklistDialog::ActionPossible(SynchronizeChecklist::ActionType Action) const
{
  if (FTotals[Action] > 0)
  {
    // if direction is overriden to an action that is otherwise not possible
    // in given synchronization mode, we still want to show the stats
    return true;
  }
  bool Timestamp = (FParams & spTimestamp) != 0;
  bool AnyUpdate = ((FParams & spNotByTime) == 0) || ((FParams & spBySize) != 0) || ((FParams & spByChecksum) != 0);
  switch (Action)
  {
    case SynchronizeChecklist::saNone: return true;
    case SynchronizeChecklist::saUploadNew: return ((FMode == smRemote) || (FMode == smBoth)) && !Timestamp;
    case SynchronizeChecklist::saDownloadNew: return ((FMode == smLocal) || (FMode == smBoth)) && !Timestamp;
    case SynchronizeChecklist::saUploadUpdate: return ((FMode == smRemote) || (FMode == smBoth)) && AnyUpdate;
    case SynchronizeChecklist::saDownloadUpdate: return ((FMode == smLocal) || (FMode == smBoth)) && AnyUpdate;
    case SynchronizeChecklist::saDeleteRemote: return (FMode == smRemote) && !Timestamp;
    case SynchronizeChecklist::saDeleteLocal: return (FMode == smLocal) && !Timestamp;
    default: return false;
  }
}

void SynchronizeChecklistDialog::UpdateStatusBar()
{
  for (int Index = 0; Index <= SynchronizeChecklist::ActionCount; Index++)
  {
    auto Action = static_cast<SynchronizeChecklist::ActionType>(Index);
    QString PanelText;
    bool Possible = ActionPossible(Action);
    if (Possible)
    {
      // SYNCHRONIZE_SELECTED_ACTIONS
      PanelText = QStringLiteral("%1 of %2").arg(QLocale().toString(FChecked[Index]), QLocale().toString(FTotals[Index]));
      if ((FChecked[Index] > 0) && ((Index == 0) || !SynchronizeChecklist::IsItemSizeIrrelevant(Action)))
      {
        PanelText += QStringLiteral(" (%1)").arg(FileListModel::FormatBytes(FCheckedSize[Index]));
      }
    }
    else
    {
      PanelText = QStringLiteral("N/A");
    }
    FPanels[Index]->setText(PanelText);
    FPanels[Index]->setEnabled(Possible && !FSynchronizing);
  }
}

void SynchronizeChecklistDialog::UpdateCaption()
{
  // SYNCHRONIZE_PROGRESS_SYNCHRONIZE2 / SYNCHRONIZE_CHECKLIST_CAPTION
  setWindowTitle(FSynchronizing ? QStringLiteral("Synchronizing") : QStringLiteral("Synchronization checklist"));
}

void SynchronizeChecklistDialog::UpdateControls()
{
  UpdateCaption();
  UpdateStatusBar();
  bool AllChecked = true;
  bool AllUnchecked = true;
  bool AnyDirectory = false;
  MoveData AMoveData(false);
  const QList<QTreeWidgetItem *> Selected = FListView->selectedItems();
  for (QTreeWidgetItem * Item : Selected)
  {
    const SynchronizeChecklist::Item * CI = ChecklistItem(Item);
    AMoveData.Collect(CI, ItemAction(CI));
    if (Item->checkState(ColName) == Qt::Checked)
    {
      AllUnchecked = false;
    }
    else
    {
      AllChecked = false;
    }
    if (CI->IsDirectory)
    {
      AnyDirectory = true;
    }
  }
  bool Idle = !FSynchronizing && !FBusy;
  int SelCount = Selected.size();
  FOkButton->setEnabled((FChecked[0] > 0) && Idle);
  FCancelButton->setEnabled(Idle);
  FCheckAction->setEnabled(!AllChecked && Idle);
  FUncheckAction->setEnabled(!AllUnchecked && Idle);
  FCheckAllAction->setEnabled((FChecked[0] < FTotals[0]) && Idle);
  FUncheckAllAction->setEnabled((FChecked[0] > 0) && Idle);
  FCheckDirectoryAction->setEnabled(FCheckAllAction->isEnabled()); // sic
  FUncheckDirectoryAction->setEnabled(FUncheckAllAction->isEnabled()); // sic
  FReverseAction->setEnabled((SelCount > 0) && Idle);
  FMoveAction->setEnabled(
    Idle && static_cast<bool>(FCallbacks.Move) &&
    // All actions are of exactly two and opposite types
    (AMoveData.SecondAction.Action != SynchronizeChecklist::saNone) &&
    !AMoveData.ThreeActions &&
    (SynchronizeChecklist::OppositeMoveAction(AMoveData.FirstAction.Action) == AMoveData.SecondAction.Action) &&
    // ... and there are exactly two of them of the same file/dir type
    (((SelCount == 2) && (AMoveData.FirstAction.AllItemsFiles == AMoveData.SecondAction.AllItemsFiles)) ||
    // ... or the "transfer new" side is exactly one directory
     AMoveData.FirstAction.IsOnlyDirectoryTransferAction() ||
     AMoveData.SecondAction.IsOnlyDirectoryTransferAction()));
  FCalculateSizeAction->setEnabled((SelCount > 0) && AnyDirectory && Idle && static_cast<bool>(FCallbacks.CalculateSize));
  FCalculateSizeAllAction->setEnabled((FDirectories > 0) && Idle && static_cast<bool>(FCallbacks.CalculateSize));
  SynchronizeChecklist::ActionType SelectedItemAction =
    (SelCount == 1) ? ItemAction(ChecklistItem(Selected.first())) : SynchronizeChecklist::saNone;
  FExploreLocalAction->setEnabled((SelCount == 1) && (SelectedItemAction != SynchronizeChecklist::saDeleteRemote));
  FExploreRemoteAction->setEnabled((SelCount == 1) && (SelectedItemAction != SynchronizeChecklist::saDeleteLocal) &&
    static_cast<bool>(FCallbacks.ExploreRemote));
  FLocalPathToClipboardAction->setEnabled(SelCount > 0);
  FRemotePathToClipboardAction->setEnabled(SelCount > 0);
  int Count = FListView->topLevelItemCount();
  FSelectAllAction->setEnabled((SelCount < Count) && Idle);
  FFindMoveCandidateAction->setEnabled((Count > 0) && Idle);
}

void SynchronizeChecklistDialog::CheckSelected(bool Check)
{
  FMass = true;
  for (QTreeWidgetItem * Item : FListView->selectedItems())
  {
    Item->setCheckState(ColName, Check ? Qt::Checked : Qt::Unchecked);
  }
  FMass = false;
  CountTotals();
  UpdateControls();
}

void SynchronizeChecklistDialog::CheckAll(bool Check)
{
  FMass = true;
  for (int Index = 0; Index < FListView->topLevelItemCount(); Index++)
  {
    FListView->topLevelItem(Index)->setCheckState(ColName, Check ? Qt::Checked : Qt::Unchecked);
  }
  FMass = false;
  CountTotals();
  UpdateControls();
}

void SynchronizeChecklistDialog::CheckDirectory(bool Check)
{
  QStringList Directories;
  for (QTreeWidgetItem * Item : FListView->selectedItems())
  {
    // It does not matter if we use local or remote directory
    Directories.append(ChecklistItem(Item)->Info1.Directory + QLatin1Char('/'));
  }
  FMass = true;
  for (int Index = 0; Index < FListView->topLevelItemCount(); Index++)
  {
    QTreeWidgetItem * Item = FListView->topLevelItem(Index);
    QString Directory = ChecklistItem(Item)->Info1.Directory + QLatin1Char('/');
    for (const QString & D : Directories)
    {
      if (Directory.startsWith(D, Qt::CaseInsensitive))
      {
        Item->setCheckState(ColName, Check ? Qt::Checked : Qt::Unchecked);
      }
    }
  }
  FMass = false;
  CountTotals();
  UpdateControls();
}

void SynchronizeChecklistDialog::Reverse()
{
  FMass = true;
  for (QTreeWidgetItem * Item : FListView->selectedItems())
  {
    SynchronizeChecklist::ActionType & Action = ItemAction(ChecklistItem(Item));
    SynchronizeChecklist::ActionType NewAction = SynchronizeChecklist::Reverse(Action);
    if (NewAction != Action)
    {
      Action = NewAction;
      LoadItem(Item);
    }
  }
  FMass = false;
  CountTotals();
  UpdateControls();
}

void SynchronizeChecklistDialog::SelectAll(int Action)
{
  QTreeWidgetItem * First = nullptr;
  FListView->blockSignals(true);
  for (int Index = 0; Index < FListView->topLevelItemCount(); Index++)
  {
    QTreeWidgetItem * Item = FListView->topLevelItem(Index);
    bool Select = (Action == 0) || (ItemAction(ChecklistItem(Item)) == Action);
    Item->setSelected(Select);
    if (Select && (First == nullptr))
    {
      First = Item;
    }
  }
  FListView->blockSignals(false);
  if (First != nullptr)
  {
    FListView->setCurrentItem(First, ColName, QItemSelectionModel::NoUpdate);
    FListView->scrollToItem(First);
  }
  FListView->setFocus();
  UpdateControls();
}

void SynchronizeChecklistDialog::ProcessedItem(const SynchronizeChecklist::Item * AChecklistItem)
{
  // We can get multiple triggers for the same item: uncheck on the first one
  auto It = FChecklistToListViewMap.find(AChecklistItem);
  if (It != FChecklistToListViewMap.end())
  {
    It->second->setCheckState(ColName, Qt::Unchecked);
    FListView->scrollToItem(It->second);
  }
}

void SynchronizeChecklistDialog::DoSynchronize(bool Queue)
{
  if (FSynchronizing || FBusy || (FChecked[0] == 0))
  {
    return;
  }
  if (Queue && !FCallbacks.QueueSynchronize)
  {
    QApplication::beep();
    return;
  }
  FListView->clearSelection();
  for (int Index = 0; Index < FListView->topLevelItemCount(); Index++)
  {
    QTreeWidgetItem * Item = FListView->topLevelItem(Index);
    const SynchronizeChecklist::Item * CI = ChecklistItem(Item);
    FChecklist->Update(CI, (Item->checkState(ColName) == Qt::Checked), ItemAction(CI));
  }
  SaveWindowParams();
  if (Queue)
  {
    FQueued = true;
    FCallbacks.QueueSynchronize();
    QDialog::accept();
    return;
  }
  if (!FCallbacks.Synchronize)
  {
    QDialog::accept();
    return;
  }
  FSynchronizing = true;
  UpdateControls();
  QPointer<SynchronizeChecklistDialog> Self(this);
  FCallbacks.Synchronize(
    [Self](const SynchronizeChecklist::Item * AChecklistItem)
    {
      if (!Self.isNull())
      {
        Self->ProcessedItem(AChecklistItem);
      }
    },
    [Self]()
    {
      if (!Self.isNull())
      {
        Self->FSynchronizing = false;
        Self->QDialog::accept();
      }
    });
}

void SynchronizeChecklistDialog::CalculateSize(bool All)
{
  if (!FCallbacks.CalculateSize || FBusy || FSynchronizing)
  {
    return;
  }
  std::vector<const SynchronizeChecklist::Item *> Items;
  QList<QTreeWidgetItem *> Source;
  if (All)
  {
    for (int Index = 0; Index < FListView->topLevelItemCount(); Index++)
    {
      Source.append(FListView->topLevelItem(Index));
    }
  }
  else
  {
    Source = FListView->selectedItems();
  }
  for (QTreeWidgetItem * Item : Source)
  {
    const SynchronizeChecklist::Item * CI = ChecklistItem(Item);
    if (CI->IsDirectory)
    {
      Items.push_back(CI);
    }
  }
  if (Items.empty())
  {
    return;
  }
  FBusy = true;
  UpdateControls();
  QApplication::setOverrideCursor(Qt::BusyCursor);
  QPointer<SynchronizeChecklistDialog> Self(this);
  FCallbacks.CalculateSize(Items, [Self, Items]()
  {
    QApplication::restoreOverrideCursor();
    if (Self.isNull())
    {
      return;
    }
    // UpdatedSynchronizationChecklistItems
    for (const SynchronizeChecklist::Item * CI : Items)
    {
      auto It = Self->FChecklistToListViewMap.find(CI);
      if (It != Self->FChecklistToListViewMap.end())
      {
        Self->LoadItem(It->second);
      }
    }
    Self->FBusy = false;
    Self->CountTotals();
    Self->UpdateControls();
  });
}

void SynchronizeChecklistDialog::DeleteItem(const SynchronizeChecklist::Item * AChecklistItem)
{
  auto It = FChecklistToListViewMap.find(AChecklistItem);
  if (It == FChecklistToListViewMap.end())
  {
    return;
  }
  QTreeWidgetItem * Item = It->second;
  FChecklistToListViewMap.erase(It);
  FActions.erase(AChecklistItem);
  if (AChecklistItem->IsDirectory)
  {
    FDirectories--;
  }
  if (FListView->currentItem() == Item)
  {
    int Index = FListView->indexOfTopLevelItem(Item);
    int FocusIndex = (Index == FListView->topLevelItemCount() - 1) ? Index - 1 : Index + 1;
    QTreeWidgetItem * Focus = FListView->topLevelItem(FocusIndex);
    if (Focus != nullptr)
    {
      FListView->setCurrentItem(Focus, ColName, QItemSelectionModel::NoUpdate);
    }
  }
  FMass = true;
  delete Item;
  FMass = false;
  FChecklist->Delete(AChecklistItem);
}

void SynchronizeChecklistDialog::Move()
{
  // TSynchronizeChecklistDialog::MoveActionExecute
  if (!FMoveAction->isEnabled())
  {
    return;
  }
  auto AMoveData = std::make_shared<MoveData>(true);
  for (QTreeWidgetItem * Item : FListView->selectedItems())
  {
    const SynchronizeChecklist::Item * CI = ChecklistItem(Item);
    AMoveData->Collect(CI, ItemAction(CI));
  }
  MoveActionData * TransferAction = &AMoveData->FirstAction;
  MoveActionData * DeleteAction = &AMoveData->SecondAction;
  if (SynchronizeChecklist::IsTransferNewAction(DeleteAction->Action))
  {
    std::swap(TransferAction, DeleteAction);
  }
  if (!SynchronizeChecklist::IsTransferNewAction(TransferAction->Action) ||
      (SynchronizeChecklist::OppositeMoveAction(DeleteAction->Action) != TransferAction->Action) ||
      (TransferAction->ChecklistItems.size() != 1))
  {
    return;
  }
  const SynchronizeChecklist::Item * TransferChecklistItem = TransferAction->ChecklistItems[0];
  bool Remote;
  QString NewFileName;
  if (DeleteAction->Action == SynchronizeChecklist::saDeleteRemote)
  {
    Remote = true;
    NewFileName = UnixPath::Combine(TransferChecklistItem->Info2.Directory, TransferChecklistItem->Info1.FileName);
  }
  else
  {
    Remote = false;
    QString Directory = TransferChecklistItem->Info1.Directory;
    NewFileName = Directory.endsWith(QLatin1Char('/')) ? Directory + TransferChecklistItem->Info2.FileName :
      Directory + QLatin1Char('/') + TransferChecklistItem->Info2.FileName;
  }
  bool TargetIsDirectory;
  if ((TransferAction->AllItemsDirectories == DeleteAction->AllItemsDirectories) &&
      (TransferAction->ItemsCount == 1) && (DeleteAction->ItemsCount == 1))
  {
    TargetIsDirectory = false;
  }
  else if (TransferAction->AllItemsDirectories && (TransferAction->ItemsCount == 1))
  {
    TargetIsDirectory = true;
  }
  else
  {
    return;
  }
  std::vector<const SynchronizeChecklist::Item *> DeleteItems = DeleteAction->ChecklistItems;
  FBusy = true;
  UpdateControls();
  QPointer<SynchronizeChecklistDialog> Self(this);
  FCallbacks.Move(Remote, DeleteAction->FileList, NewFileName, TargetIsDirectory,
    [Self, DeleteItems, TransferChecklistItem, TargetIsDirectory, AMoveData](bool Ok)
    {
      if (Self.isNull())
      {
        return;
      }
      Self->FBusy = false;
      if (Ok)
      {
        for (const SynchronizeChecklist::Item * CI : DeleteItems)
        {
          Self->DeleteItem(CI);
        }
        if (!TargetIsDirectory)
        {
          Self->DeleteItem(TransferChecklistItem);
        }
        else
        {
          // The remaining "transfer" item
          auto It = Self->FChecklistToListViewMap.find(TransferChecklistItem);
          if (It != Self->FChecklistToListViewMap.end())
          {
            Self->FListView->scrollToItem(It->second);
          }
        }
      }
      Self->CountTotals();
      Self->UpdateControls();
    });
}

void SynchronizeChecklistDialog::FindMoveCandidate()
{
  // TSynchronizeChecklistDialog::FindMoveCandidateActionExecute
  std::map<QString, std::vector<const SynchronizeChecklist::Item *>> CandidatesFileName;
  std::map<qint64, std::vector<const SynchronizeChecklist::Item *>> CandidatesSize;
  int Count = FListView->topLevelItemCount();
  for (int Index = 0; Index < Count; Index++)
  {
    const SynchronizeChecklist::Item * CI = ChecklistItem(FListView->topLevelItem(Index));
    if (CI->IsLocalOnly() || CI->IsRemoteOnly())
    {
      if (CI->IsDirectory)
      {
        CandidatesFileName[CI->GetFileName().toLower()].push_back(CI);
      }
      else
      {
        CandidatesSize[CI->GetBaseSize()].push_back(CI);
      }
    }
  }

  bool Found = false;
  if ((Count > 0) && (!CandidatesFileName.empty() || !CandidatesSize.empty()))
  {
    QTreeWidgetItem * ItemFocused = FListView->currentItem();
    QTreeWidgetItem * Item = (ItemFocused != nullptr) ? ItemFocused : FListView->topLevelItem(0);
    QTreeWidgetItem * FirstItem = nullptr;
    const QList<QTreeWidgetItem *> SelectedItems = FListView->selectedItems();
    while (!Found && (Item != nullptr))
    {
      const SynchronizeChecklist::Item * CI = ChecklistItem(Item);
      SynchronizeChecklist::ActionType Action = ItemAction(CI);
      SynchronizeChecklist::ActionType OppositeAction = SynchronizeChecklist::OppositeMoveAction(Action);
      if ((OppositeAction != SynchronizeChecklist::saNone) &&
          // For focused item, we search pair even if the focused item is "delete" action,
          // but when searching the next items, consider "transfer" actions only
          ((Item == ItemFocused) || SynchronizeChecklist::IsTransferNewAction(Action)))
      {
        std::vector<const SynchronizeChecklist::Item *> Candidates;
        if (CI->IsDirectory)
        {
          auto I = CandidatesFileName.find(CI->GetFileName().toLower());
          if (I != CandidatesFileName.end())
          {
            for (const SynchronizeChecklist::Item * CI2 : I->second)
            {
              if ((ItemAction(CI2) == OppositeAction) && CI2->IsDirectory)
              {
                Candidates.push_back(CI2);
              }
            }
          }
        }
        else
        {
          auto I = CandidatesSize.find(CI->GetBaseSize());
          if (I != CandidatesSize.end())
          {
            QString FileName = CI->GetFileName();
            for (const SynchronizeChecklist::Item * CI2 : I->second)
            {
              if ((ItemAction(CI2) == OppositeAction) && !CI2->IsDirectory)
              {
                bool IsCandidate;
                // in addition to the same size, also the same file filename (although possibly different directory)
                if (FileName.compare(CI2->GetFileName(), Qt::CaseInsensitive) == 0)
                {
                  IsCandidate = true;
                }
                // or different filename but the same directory (in addition to the same size)
                else if ((Action == SynchronizeChecklist::saDeleteLocal) || (Action == SynchronizeChecklist::saDownloadNew))
                {
                  IsCandidate = (QDir::cleanPath(CI->Info1.Directory) == QDir::cleanPath(CI2->Info1.Directory));
                }
                else
                {
                  IsCandidate = (CI->Info2.Directory == CI2->Info2.Directory);
                }
                if (IsCandidate)
                {
                  Candidates.push_back(CI2);
                }
              }
            }
          }
        }

        if (!Candidates.empty())
        {
          const SynchronizeChecklist::Item * CI2 = Candidates[0];
          if ((FirstItem == nullptr) && Item->isSelected() && (SelectedItems.size() == 2))
          {
            QTreeWidgetItem * NextSelected = (SelectedItems[0] == Item) ? SelectedItems[1] : SelectedItems[0];
            auto I = std::find(Candidates.begin(), Candidates.end(), ChecklistItem(NextSelected));
            if ((I != Candidates.end()) && (I < Candidates.end() - 1))
            {
              CI2 = *(I + 1);
            }
            else if ((I != Candidates.end()) && (I == Candidates.end() - 1))
            {
              CI2 = nullptr;
            }
          }

          if (CI2 != nullptr)
          {
            QTreeWidgetItem * Item2 = FChecklistToListViewMap[CI2];
            FListView->blockSignals(true);
            for (int Index = 0; Index < Count; Index++)
            {
              QTreeWidgetItem * ItemI = FListView->topLevelItem(Index);
              ItemI->setSelected((ItemI == Item) || (ItemI == Item2));
            }
            FListView->blockSignals(false);
            FListView->setCurrentItem(Item, ColName, QItemSelectionModel::NoUpdate);
            FListView->scrollToItem(Item2);
            FListView->scrollToItem(Item);
            Found = true;
          }
        }
      }

      // Allow going through the first item twice, to make sure we roll over its previous pair candidates
      if (Item == FirstItem)
      {
        Item = nullptr;
      }
      else
      {
        if (FirstItem == nullptr)
        {
          FirstItem = Item;
        }
        int Index = FListView->indexOfTopLevelItem(Item) + 1;
        Item = FListView->topLevelItem((Index < Count) ? Index : 0);
      }
    }
  }

  if (!Found)
  {
    QApplication::beep();
  }
  UpdateControls();
}

void SynchronizeChecklistDialog::DoExplore(bool Local)
{
  // TCustomScpExplorerForm::DoSynchronizeExplore
  const QList<QTreeWidgetItem *> Selected = FListView->selectedItems();
  if (Selected.size() != 1)
  {
    return;
  }
  const SynchronizeChecklist::Item * CI = ChecklistItem(Selected.first());
  SynchronizeChecklist::ActionType Action = ItemAction(CI);
  if (Local)
  {
    if (Action == SynchronizeChecklist::saDownloadNew)
    {
      ExploreLocalPath(CI->Info1.Directory, true);
    }
    else
    {
      ExploreLocalPath(CI->ForceGetLocalPath(), false);
    }
  }
  else if (FCallbacks.ExploreRemote)
  {
    FCallbacks.ExploreRemote(CI->Info2.Directory, (Action == SynchronizeChecklist::saUploadNew) ? QString() : CI->GetFileName());
  }
}

void SynchronizeChecklistDialog::PathToClipboard(bool Local)
{
  QStringList Paths;
  for (QTreeWidgetItem * Item : FListView->selectedItems())
  {
    const SynchronizeChecklist::Item * CI = ChecklistItem(Item);
    Paths.append(Local ? CI->ForceGetLocalPath() : CI->ForceGetRemotePath());
  }
  if (!Paths.isEmpty())
  {
    // CopyToClipboard of a string list: one path per line
    QApplication::clipboard()->setText(Paths.join(QLatin1Char('\n')) + ((Paths.size() > 1) ? QStringLiteral("\n") : QString()));
  }
}

void SynchronizeChecklistDialog::LoadListParams(const QString & ListParams)
{
  // TCustomListViewColProperties::SetParamsStr:
  // "SortColumn;SortAscending|Width,Visible;...;@PixelsPerInch|Order"
  QStringList Sections = ListParams.split(QLatin1Char('|'));
  QStringList Sort = Sections.value(0).split(QLatin1Char(';'));
  QStringList Columns = Sections.value(1).split(QLatin1Char(';'));
  int PixelsPerInch = 96;
  if (!Columns.isEmpty() && Columns.last().startsWith(QLatin1Char('@')))
  {
    PixelsPerInch = Columns.takeLast().mid(1).toInt();
  }
  double Scale = ((PixelsPerInch > 0) && (logicalDpiX() > 0)) ? static_cast<double>(logicalDpiX()) / PixelsPerInch : 1.0;
  static const int DefaultWidths[] = {150, 100, 80, 130, 25, 100, 80, 130};
  for (int Column = 0; Column < 8; Column++)
  {
    QStringList Parts = Columns.value(Column).split(QLatin1Char(','));
    bool Ok = false;
    int Width = Parts.value(0).toInt(&Ok);
    if (!Ok || (Width <= 0))
    {
      Width = DefaultWidths[Column];
    }
    FListView->setColumnWidth(Column, std::max(16, static_cast<int>(Width * Scale)));
    FListView->setColumnHidden(Column, (Column != ColName) && Ok && (Parts.value(1) == QStringLiteral("0")));
  }
  FListView->setColumnWidth(ColAction, std::max(FListView->columnWidth(ColAction), 28));
  QStringList Order = Sections.value(2).split(QLatin1Char(';'), Qt::SkipEmptyParts);
  if (Order.size() == 8)
  {
    for (int Visual = 0; Visual < 8; Visual++)
    {
      bool Ok = false;
      int Logical = Order.at(Visual).toInt(&Ok);
      if (Ok && (Logical >= 0) && (Logical < 8))
      {
        FListView->header()->moveSection(FListView->header()->visualIndex(Logical), Visual);
      }
    }
  }
  bool SortOk = false;
  int SortColumn = Sort.value(0).toInt(&SortOk);
  if (SortOk && (SortColumn >= 0) && (SortColumn < 8) && (SortColumn != ColAction) && (Sort.size() >= 2))
  {
    // The default ("1;1") keeps the checklist order (by directory and name)
    if (ListParams.section(QLatin1Char('|'), 0, 0) != QStringLiteral("1;1"))
    {
      FListView->header()->setSortIndicatorShown(true);
      FListView->header()->setSortIndicator(SortColumn, (Sort.value(1) == QStringLiteral("0")) ? Qt::DescendingOrder : Qt::AscendingOrder);
    }
  }
}

QString SynchronizeChecklistDialog::StoreListParams() const
{
  QHeaderView * Header = FListView->header();
  QString Result = Header->isSortIndicatorShown() ?
    QStringLiteral("%1;%2").arg(Header->sortIndicatorSection()).arg((Header->sortIndicatorOrder() == Qt::AscendingOrder) ? 1 : 0) :
    QStringLiteral("1;1");
  Result += QLatin1Char('|');
  for (int Column = 0; Column < 8; Column++)
  {
    Result += QStringLiteral("%1,%2;").arg(FListView->columnWidth(Column)).arg(FListView->isColumnHidden(Column) ? 0 : 1);
  }
  Result += QStringLiteral("@%1|").arg(logicalDpiX());
  QStringList Order;
  for (int Visual = 0; Visual < 8; Visual++)
  {
    Order.append(QString::number(Header->logicalIndex(Visual)));
  }
  return Result + Order.join(QLatin1Char(';'));
}

void SynchronizeChecklistDialog::SaveWindowParams()
{
  // TSynchronizeChecklistDialog::Execute (when accepted)
  SynchronizeConfiguration Config = SynchronizeConfiguration::Load();
  Config.ChecklistListParams = StoreListParams();
  Config.ChecklistWindowParams = StoreWindowParams(this, FMainWindow, Config.ChecklistWindowParams);
  Config.Save();
}

void SynchronizeChecklistDialog::accept()
{
  // Enter in the list = OK
  DoSynchronize(false);
}

void SynchronizeChecklistDialog::reject()
{
  if (FSynchronizing || FBusy)
  {
    return;
  }
  QDialog::reject();
}

void SynchronizeChecklistDialog::closeEvent(QCloseEvent * Event)
{
  if (FSynchronizing || FBusy)
  {
    Event->ignore();
    return;
  }
  QDialog::closeEvent(Event);
}

//---------------------------------------------------------------------------
// Full synchronization (TCustomScpExplorerForm::DoFullSynchronizeDirectories / FullSynchronize)

struct FullSynchronizeRun : public std::enable_shared_from_this<FullSynchronizeRun>
{
  QPointer<QWidget> Parent;
  QPointer<Terminal> Term;
  QString Directory1;
  QString Directory2;
  SynchronizeMode Mode = smRemote;
  int Params = 0;
  CopyParam Param;
  SynchronizeOptions Options;
  std::function<void()> Finished;
  QElapsedTimer CollectTimer;
  qint64 CollectElapsed = 0;
  QElapsedTimer SynchronizeTimer;
  bool DeleteLocalToTrash = false;

  void Collect();
  void Collected(const OperationResult & Result, SynchronizeChecklistPtr Checklist);
  void Synchronize(SynchronizeChecklistPtr Checklist, QWidget * ProgressParent, SynchronizeProcessedItemEvent OnProcessedItem,
    std::function<void(const OperationResult &, const SynchronizeStatistics &)> AFinished);
  void Synchronized(const OperationResult & Result, const SynchronizeStatistics & Statistics);
  void Done()
  {
    std::function<void()> AFinished = std::move(Finished);
    Finished = nullptr;
    if (AFinished)
    {
      AFinished();
    }
  }
  void ShowError(const OperationResult & Result)
  {
    // Fatal errors are reported by the main window (Terminal::Disconnected)
    if (!Result.Ok && !Result.Aborted && !Result.Fatal)
    {
      ShowErrorMessage(Parent, Result.Message, Result.MoreMessages);
    }
  }
};

void FullSynchronizeRun::Collect()
{
  auto Self = shared_from_this();
  auto Status = std::make_shared<SynchronizeStatus>();
  auto * Progress = new SynchronizeProgressDialog(Term, Status, nullptr, Parent);
  Progress->setAttribute(Qt::WA_DeleteOnClose, false);
  CollectTimer.start();
  Progress->Start();
  SynchronizeCollect(Term, Directory1, Directory2, Mode, Param, Params | spNoConfirmation, Options, Status,
    [Self, Progress](const OperationResult & Result, SynchronizeChecklistPtr Checklist)
    {
      Progress->Finish();
      Progress->deleteLater();
      Self->CollectElapsed = Self->CollectTimer.elapsed();
      Self->Collected(Result, Checklist);
    });
}

void FullSynchronizeRun::Collected(const OperationResult & Result, SynchronizeChecklistPtr Checklist)
{
  if (!Result.Ok || Term.isNull())
  {
    ShowError(Result);
    Done();
    return;
  }
  if (Checklist->Count() == 0)
  {
    // COMPARE_NO_DIFFERENCES, HELP_SYNCHRONIZE_NO_DIFFERENCES
    MessageDialog(Parent, QStringLiteral("No differences found."), qtInformation, qaOK, QStringLiteral("task_synchronize_full"));
    Done();
    return;
  }
  if ((Params & spPreviewChanges) != 0)
  {
    auto Self = shared_from_this();
    auto SyncResult = std::make_shared<OperationResult>();
    auto SyncStatistics = std::make_shared<SynchronizeStatistics>();
    ChecklistCallbacks Callbacks;
    QPointer<QWidget> ProgressParent;
    Callbacks.Synchronize = [Self, Checklist, SyncResult, SyncStatistics, &ProgressParent]
      (SynchronizeProcessedItemEvent ProcessedItem, std::function<void()> ADone)
    {
      Self->Synchronize(Checklist, ProgressParent, std::move(ProcessedItem),
        [SyncResult, SyncStatistics, ADone](const OperationResult & AResult, const SynchronizeStatistics & Statistics)
        {
          *SyncResult = AResult;
          *SyncStatistics = Statistics;
          ADone();
        });
    };
    SynchronizeUiHooks & Hooks = SynchronizeHooks();
    TransferQueue * Queue = (Hooks.Queue && !Term.isNull()) ? Hooks.Queue(Term) : nullptr;
    if (Queue != nullptr)
    {
      QPointer<TransferQueue> AQueue(Queue);
      Callbacks.QueueSynchronize = [Self, Checklist, AQueue]()
      {
        // TCustomScpExplorerForm::DoQueueSynchronize
        SynchronizeToQueue(AQueue, *Checklist, Self->Param, Self->Params | spNoConfirmation, Self->DeleteLocalToTrash);
      };
    }
    Callbacks.CalculateSize = [Self, Checklist](const std::vector<const SynchronizeChecklist::Item *> & Items, std::function<void()> ADone)
    {
      // terminal can be already closed (e.g. dropped connection)
      if (Self->Term.isNull() || !Self->Term->Active())
      {
        ADone();
        return;
      }
      SynchronizeChecklistCalculateSize(Self->Term, Checklist, Items, Self->Param, Self->Params,
        [Self, ADone](const OperationResult & AResult)
        {
          Self->ShowError(AResult);
          ADone();
        });
    };
    Callbacks.Move = [Self](bool Remote, const QStringList & FileList, const QString & NewFileName, bool TargetIsDirectory,
      std::function<void(bool)> ADone)
    {
      if (Self->Term.isNull() || !Self->Term->Active())
      {
        ADone(false);
        return;
      }
      SynchronizeMove(Self->Term, Remote, FileList, NewFileName, TargetIsDirectory,
        [Self, ADone](const OperationResult & AResult)
        {
          Self->ShowError(AResult);
          ADone(AResult.Ok);
        });
    };
    if (Hooks.ExploreRemote)
    {
      Callbacks.ExploreRemote = [Self](const QString & RemoteDirectory, const QString & FileName)
      {
        if (!Self->Term.isNull() && SynchronizeHooks().ExploreRemote)
        {
          SynchronizeHooks().ExploreRemote(Self->Term, RemoteDirectory, FileName);
        }
      };
    }
    SynchronizeChecklistDialog Dialog(Checklist, Mode, Params, Directory1, Directory2, Callbacks, Parent);
    ProgressParent = &Dialog;
    int DialogResult = Dialog.exec();
    if (DialogResult != QDialog::Accepted)
    {
      Done();
      return;
    }
    if (Dialog.Queued())
    {
      Done();
      return;
    }
    Synchronized(*SyncResult, *SyncStatistics);
    return;
  }
  if (Term.isNull() || !Term->Active())
  {
    Done();
    return;
  }
  auto Self = shared_from_this();
  Synchronize(Checklist, Parent, nullptr,
    [Self](const OperationResult & AResult, const SynchronizeStatistics & Statistics) { Self->Synchronized(AResult, Statistics); });
}

void FullSynchronizeRun::Synchronize(SynchronizeChecklistPtr Checklist, QWidget * ProgressParent, SynchronizeProcessedItemEvent OnProcessedItem,
  std::function<void(const OperationResult &, const SynchronizeStatistics &)> AFinished)
{
  // Finished exactly once, also when the session is destroyed meanwhile
  auto Fired = std::make_shared<bool>(false);
  auto Finish = [Fired, AFinished](const OperationResult & Result, const SynchronizeStatistics & Statistics)
  {
    if (!*Fired)
    {
      *Fired = true;
      AFinished(Result, Statistics);
    }
  };
  if (Term.isNull() || !Term->Active())
  {
    OperationResult Result;
    Result.Ok = false;
    Result.Aborted = true;
    Finish(Result, SynchronizeStatistics());
    return;
  }
  auto Status = std::make_shared<SynchronizeStatus>();
  QPointer<SynchronizeProgressDialog> Progress(new SynchronizeProgressDialog(Term, Status, Checklist, ProgressParent));
  QObject::connect(Term, &QObject::destroyed, Progress.data(), [Progress, Finish]()
  {
    OperationResult Result;
    Result.Ok = false;
    Result.Fatal = true;
    if (!Progress.isNull())
    {
      Progress->Finish();
      Progress->deleteLater();
    }
    Finish(Result, SynchronizeStatistics());
  });
  SynchronizeTimer.start();
  Progress->Start();
  SynchronizeApply(Term, Checklist, Param, Params | spNoConfirmation, Status,
    [Progress, OnProcessedItem](const SynchronizeChecklist::Item * ChecklistItem)
    {
      if (!Progress.isNull())
      {
        Progress->ItemProcessed(ChecklistItem);
      }
      if (OnProcessedItem)
      {
        OnProcessedItem(ChecklistItem);
      }
    },
    [Progress, Finish](const OperationResult & Result, const SynchronizeStatistics & Statistics)
    {
      if (!Progress.isNull())
      {
        Progress->Finish();
        Progress->deleteLater();
      }
      Finish(Result, Statistics);
    },
    DeleteLocalToTrash);
}

void FullSynchronizeRun::Synchronized(const OperationResult & Result, const SynchronizeStatistics & Statistics)
{
  if (!Result.Ok)
  {
    ShowError(Result);
    Done();
    return;
  }
  Configuration & Config = Configuration::Instance();
  if (Config.SynchronizeSummary)
  {
    // SYNCHRONIZE_COMPLETE + SYNCHRONIZE_SUMMARY
    QStringList Lines;
    if ((Params & spTimestamp) == 0)
    {
      QLocale Locale;
      if (Statistics.FilesUploaded > 0)
      {
        Lines.append(QStringLiteral("Files uploaded: %1 (%2)").arg(Locale.toString(Statistics.FilesUploaded), FileListModel::FormatBytes(Statistics.TotalUploaded)));
      }
      if (Statistics.FilesDownloaded > 0)
      {
        Lines.append(QStringLiteral("Files downloaded: %1 (%2)").arg(Locale.toString(Statistics.FilesDownloaded), FileListModel::FormatBytes(Statistics.TotalDownloaded)));
      }
      if (Statistics.FilesDeletedLocal > 0)
      {
        Lines.append(QStringLiteral("Local files deleted: %1").arg(Locale.toString(Statistics.FilesDeletedLocal)));
      }
      if (Statistics.FilesDeletedRemote > 0)
      {
        Lines.append(QStringLiteral("Remote files deleted: %1").arg(Locale.toString(Statistics.FilesDeletedRemote)));
      }
      if (!Lines.isEmpty())
      {
        Lines.append(QString());
      }
    }
    Lines.append(QStringLiteral("Comparison time: %1").arg(FormatDateTimeSpan(CollectElapsed)));
    Lines.append(QStringLiteral("Synchronization time: %1").arg(FormatDateTimeSpan(SynchronizeTimer.elapsed())));
    bool NeverShow = false;
    MessageWithNeverAsk(Parent, QMessageBox::Information, QStringLiteral("Synchronization was completed."),
      Lines.join(QLatin1Char('\n')), QMessageBox::Ok, QStringLiteral("Never show this message a&gain"), NeverShow,
      QString());   // HELP_NONE
    if (NeverShow)
    {
      Config.SynchronizeSummary = false;
      Config.Save();
    }
  }
  Done();
}

//---------------------------------------------------------------------------
// TSynchronizeDialog (keep remote directory up to date)

class SynchronizeDialog : public QDialog
{
public:
  SynchronizeDialog(Terminal * Term, int Options, QWidget * Parent);
  ~SynchronizeDialog() override;

  void SetParams(const SynchronizeParamType & Value);
  SynchronizeParamType Params() const;
  void SetCopyParams(const CopyParam & Value) { FCopyParams = Value; UpdateControls(); }
  CopyParam CopyParams() const;
  void SetSaveSettings(bool Value) { FSaveSettingsCheck->setChecked(Value); }
  bool SaveSettings() const { return FSaveSettingsCheck->isChecked(); }
  void SetSelectedNames(const QStringList & Names) { FSelectedNames = Names; }
  void SetChangeDelay(int Ms) { FController.SetChangeDelay(Ms); }
  void SetMaxWatchDirectories(int Value) { FMaxWatchDirectories = Value; }
  // MaxWatchDirectories changed by "Never ask me again" (0 = unchanged)
  int NewMaxWatchDirectories() const { return FNewMaxWatchDirectories; }

protected:
  void reject() override;
  void keyPressEvent(QKeyEvent * Event) override;
  bool eventFilter(QObject * Object, QEvent * Event) override;

private:
  QPointer<Terminal> FTerminal;
  int FOptions;
  SynchronizeParamType FParams;
  CopyParam FCopyParams;
  QStringList FSelectedNames;
  QString FPreset;
  bool FSynchronizing = false;
  int FMaxWatchDirectories = 500;
  int FNewMaxWatchDirectories = 0;
  SynchronizeController FController;
  QPointer<ProgressDialog> FProgress;
  QSystemTrayIcon * FTrayIcon = nullptr;
  QList<QPointer<QWidget>> FHiddenWindows;
  // TSynchronizeDialog::FMinimizedByMe: restored when the watching stops
  bool FMinimizedByMe = false;

  QComboBox * FLocalDirectoryEdit;
  QComboBox * FRemoteDirectoryEdit;
  QPushButton * FLocalDirectoryBrowseButton;
  QGroupBox * FDirectoriesGroup;
  QGroupBox * FOptionsGroup;
  QGroupBox * FCopyParamGroup;
  QCheckBox * FSynchronizeDeleteCheck;
  QCheckBox * FSynchronizeExistingOnlyCheck;
  QCheckBox * FSynchronizeRecursiveCheck;
  QCheckBox * FSynchronizeSynchronizeCheck;
  QCheckBox * FSynchronizeSelectedOnlyCheck;
  QCheckBox * FContinueOnErrorCheck;
  QCheckBox * FSaveSettingsCheck;
  CopyParamLabel * FCopyParamLabel;
  QPushButton * FTransferSettingsButton;
  QPushButton * FStartButton;
  QWidget * FStartContainer;
  QPushButton * FStopButton;
  QPushButton * FCancelButton;
  QPushButton * FMinimizeButton;
  QWidget * FMinimizeContainer;
  QTreeWidget * FLogView;

  int ActualCopyParamAttrs() const { return CopyParam::UsableAttrs(true) | cpaNoPreserveTime | cpaNoNewerOnly; }
  void UpdateControls();
  void CopyParamListPopup(const QPoint & GlobalPos);
  void CopyParamClick();
  void StartInNewWindow();
  void MinimizeToTray();
  void RestoreFromTray();
  void TooManyDirectories(int & MaxDirectories, bool & Abort);
  void Start();
  void Stop();
  void DoLog(SynchronizeLogEntry Entry, const QString & Message, const QString & MoreMessages);
  void ClearLog() { FLogView->clear(); }
  void CopyLog();
};

const int MaxLogItems = 1000;
const int soAllowSelectedOnly = 0x04;
const int LogEntryRole = Qt::UserRole + 1;
const int LogMoreRole = Qt::UserRole + 2;
const int LogMessageRole = Qt::UserRole + 3;

SynchronizeDialog::SynchronizeDialog(Terminal * Term, int Options, QWidget * Parent) :
  QDialog(Parent),
  FTerminal(Term),
  FOptions(Options),
  FController(Term)
{
  auto * Layout = new QVBoxLayout(this);

  FDirectoriesGroup = new QGroupBox(QStringLiteral("Directories"));
  auto * DirectoriesLayout = new QGridLayout(FDirectoriesGroup);
  auto * LocalDirectoryLabel = new QLabel(QStringLiteral("&Watch for changes in the local directory:"));
  FLocalDirectoryEdit = DirectoryCombo(LocalDirectoryHistory);
  LocalDirectoryLabel->setBuddy(FLocalDirectoryEdit);
  FLocalDirectoryBrowseButton = new QPushButton(QStringLiteral("B&rowse..."));
  auto * RemoteDirectoryLabel = new QLabel(QStringLiteral("... &and automatically reflect them on the remote directory:"));
  FRemoteDirectoryEdit = DirectoryCombo(RemoteDirectoryHistory);
  RemoteDirectoryLabel->setBuddy(FRemoteDirectoryEdit);
  DirectoriesLayout->addWidget(LocalDirectoryLabel, 0, 0, 1, 2);
  DirectoriesLayout->addWidget(FLocalDirectoryEdit, 1, 0);
  DirectoriesLayout->addWidget(FLocalDirectoryBrowseButton, 1, 1);
  DirectoriesLayout->addWidget(RemoteDirectoryLabel, 2, 0, 1, 2);
  DirectoriesLayout->addWidget(FRemoteDirectoryEdit, 3, 0, 1, 2);
  Layout->addWidget(FDirectoriesGroup);

  FOptionsGroup = new QGroupBox(QStringLiteral("Synchronize options"));
  auto * OptionsLayout = new QGridLayout(FOptionsGroup);
  FSynchronizeDeleteCheck = new QCheckBox(QStringLiteral("&Delete files"));
  FSynchronizeExistingOnlyCheck = new QCheckBox(QStringLiteral("&Existing files only"));
  FSynchronizeRecursiveCheck = new QCheckBox(QStringLiteral("Update s&ubdirectories"));
  FSynchronizeSynchronizeCheck = new QCheckBox(QStringLiteral("Synchronize on s&tart"));
  FSynchronizeSynchronizeCheck->setTristate(true);
  FSynchronizeSelectedOnlyCheck = new QCheckBox(QStringLiteral("Selected files o&nly"));
  FContinueOnErrorCheck = new QCheckBox(QStringLiteral("Continue on &error"));
  FSaveSettingsCheck = new QCheckBox(QStringLiteral("Use same &options next time"));
  OptionsLayout->addWidget(FSynchronizeDeleteCheck, 0, 0);
  OptionsLayout->addWidget(FSynchronizeExistingOnlyCheck, 0, 1);
  OptionsLayout->addWidget(FSynchronizeRecursiveCheck, 1, 0);
  OptionsLayout->addWidget(FSynchronizeSynchronizeCheck, 1, 1);
  OptionsLayout->addWidget(FSynchronizeSelectedOnlyCheck, 2, 0);
  OptionsLayout->addWidget(FContinueOnErrorCheck, 2, 1);
  OptionsLayout->addWidget(FSaveSettingsCheck, 3, 0);
  Layout->addWidget(FOptionsGroup);

  FCopyParamGroup = new QGroupBox(QStringLiteral("Transfer settings"));
  auto * CopyParamLayout = new QVBoxLayout(FCopyParamGroup);
  FCopyParamLabel = new CopyParamLabel();
  CopyParamLayout->addWidget(FCopyParamLabel);
  Layout->addWidget(FCopyParamGroup);
  FCopyParamGroup->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(FCopyParamGroup, &QWidget::customContextMenuRequested, this, [this](const QPoint & Pos)
  {
    if (!FSynchronizing)
    {
      CopyParamListPopup(FCopyParamGroup->mapToGlobal(Pos));
    }
  });

  auto * Buttons = new QHBoxLayout();
  SplitButtonWidgets TransferSettings =
    SplitButton(QStringLiteral("Transfer settin&gs..."), [this](const QPoint & Pos) { CopyParamListPopup(Pos); }, this);
  FTransferSettingsButton = TransferSettings.Button;
  FTransferSettingsButton->setAutoDefault(false);
  // StartMenu: Start / Start in New Window
  SplitButtonWidgets StartSplit = SplitButton(QStringLiteral("&Start"), [this](const QPoint & Pos)
  {
    QMenu Menu(this);
    QAction * StartAction = AddDefaultMenuItem(Menu, QStringLiteral("&Start"));
    QAction * NewWindowAction = Menu.addAction(QStringLiteral("Start in &New Window"));
    QAction * Chosen = Menu.exec(Pos);
    if (Chosen == StartAction)
    {
      Start();
    }
    else if (Chosen == NewWindowAction)
    {
      StartInNewWindow();
    }
  }, this);
  FStartButton = StartSplit.Button;
  FStartContainer = StartSplit.Container;
  StartSplit.Arrow->setVisible(static_cast<bool>(SynchronizeHooks().KeepUpToDateInNewWindow));
  FStopButton = new QPushButton(QStringLiteral("&Stop"));
  // MinimizeMenu: Minimize / Minimize to System Tray
  SplitButtonWidgets MinimizeSplit = SplitButton(QStringLiteral("&Minimize"), [this](const QPoint & Pos)
  {
    QMenu Menu(this);
    QAction * MinimizeAction = AddDefaultMenuItem(Menu, QStringLiteral("&Minimize"));
    QAction * TrayAction = Menu.addAction(QStringLiteral("Minimize to System &Tray"));
    QAction * Chosen = Menu.exec(Pos);
    if (Chosen == MinimizeAction)
    {
      MinimizeApplication(this);
      FMinimizedByMe = true;
    }
    else if (Chosen == TrayAction)
    {
      MinimizeToTray();
    }
  }, this);
  FMinimizeButton = MinimizeSplit.Button;
  FMinimizeButton->setAutoDefault(false);
  FMinimizeContainer = MinimizeSplit.Container;
  // Linux: without a system tray (StatusNotifierItem host) only "Minimize"
  MinimizeSplit.Arrow->setVisible(QSystemTrayIcon::isSystemTrayAvailable());
  FCancelButton = new QPushButton(QStringLiteral("Close"));
  FCancelButton->setAutoDefault(false);
  auto * HelpButton = new QPushButton(QStringLiteral("&Help"));
  HelpButton->setAutoDefault(false);
  HelpSystem::InstallFormHelp(this, QStringLiteral("ui_keepuptodate"), HelpButton);
  Buttons->addWidget(TransferSettings.Container);
  Buttons->addStretch();
  Buttons->addWidget(FStartContainer);
  Buttons->addWidget(FStopButton);
  Buttons->addWidget(FMinimizeContainer);
  Buttons->addWidget(FCancelButton);
  Buttons->addWidget(HelpButton);
  Layout->addLayout(Buttons);

  FLogView = new QTreeWidget();
  FLogView->setColumnCount(2);
  FLogView->setHeaderHidden(true);
  FLogView->setRootIsDecorated(false);
  FLogView->setUniformRowHeights(true);
  FLogView->setSelectionMode(QAbstractItemView::ExtendedSelection);
  FLogView->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  FLogView->header()->setStretchLastSection(true);
  FLogView->setMinimumHeight(150);
  FLogView->installEventFilter(this);
  Layout->addWidget(FLogView, 1);

  FCopyParamLabel->OnClick = [this]() { CopyParamClick(); };
  connect(FTransferSettingsButton, &QPushButton::clicked, this, [this]() { CopyParamClick(); });
  connect(FMinimizeButton, &QPushButton::clicked, this, [this]() { MinimizeApplication(this); FMinimizedByMe = true; });
  connect(&FController, &SynchronizeController::TooManyDirectories, this,
    [this](int & MaxDirectories, bool & Abort) { TooManyDirectories(MaxDirectories, Abort); }, Qt::DirectConnection);
  connect(FLocalDirectoryBrowseButton, &QPushButton::clicked, this, [this]()
  {
    QString Directory = SelectLocalDirectory(this, FLocalDirectoryEdit->currentText());
    if (!Directory.isEmpty())
    {
      FLocalDirectoryEdit->setEditText(Directory);
    }
  });
  connect(FLocalDirectoryEdit, &QComboBox::editTextChanged, this, [this]() { UpdateControls(); });
  connect(FRemoteDirectoryEdit, &QComboBox::editTextChanged, this, [this]() { UpdateControls(); });
  connect(FStartButton, &QPushButton::clicked, this, [this]() { Start(); });
  connect(FStopButton, &QPushButton::clicked, this, [this]() { Stop(); });
  connect(FCancelButton, &QPushButton::clicked, this, &QDialog::reject);
  connect(FLogView, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem * Item)
  {
    if (Item->data(0, LogEntryRole).toInt() == slContinuedError)
    {
      ShowErrorMessage(this, Item->data(0, LogMessageRole).toString(), Item->data(0, LogMoreRole).toString());
    }
  });

  connect(&FController, &SynchronizeController::Log, this,
    [this](SynchronizeLogEntry Entry, const QString & Message, const QString & MoreMessages) { DoLog(Entry, Message, MoreMessages); });
  connect(&FController, &SynchronizeController::Aborted, this,
    [this](bool Close, const QString & Message, const QString & MoreMessages)
    {
      // TSynchronizeDialog::Abort / DoSynchronizeInvalid
      if (FSynchronizing)
      {
        Stop();
      }
      if (!Message.isEmpty() && !Close)
      {
        ShowErrorMessage(this, Message, MoreMessages);
      }
      if (Close)
      {
        QDialog::reject();
      }
    });
  connect(&FController, &SynchronizeController::BusyChanged, this, [this](bool Busy)
  {
    if (Busy && !FTerminal.isNull() && FProgress.isNull())
    {
      // progress of the transfers (shown only when they take a while)
      FProgress = new ProgressDialog(FTerminal->Progress(), this);
      FProgress->Start();
    }
    else if (!Busy && !FProgress.isNull())
    {
      FProgress->Finish();
      FProgress->deleteLater();
    }
  });

  UpdateControls();
  resize(560, sizeHint().height());
}

SynchronizeDialog::~SynchronizeDialog()
{
  if (FSynchronizing)
  {
    FController.Stop();
  }
  RestoreFromTray();
}

void SynchronizeDialog::CopyParamClick()
{
  if (FSynchronizing)
  {
    return;
  }
  CopyParam ACopyParams = CopyParams();
  if (CopyParamsDialog::Execute(ACopyParams, ActualCopyParamAttrs(), this))
  {
    FCopyParams = ACopyParams;
    UpdateControls();
  }
}

void SynchronizeDialog::CopyParamListPopup(const QPoint & GlobalPos)
{
  CopyParam ACopyParams = FCopyParams;
  if (::CopyParamListPopup(this, GlobalPos, ACopyParams, FPreset, cplCustomizeDefault, ActualCopyParamAttrs()) > 0)
  {
    FCopyParams = ACopyParams;
  }
  UpdateControls();
}

void SynchronizeDialog::StartInNewWindow()
{
  // TSynchronizeDialog::StartInNewWindow: the new window keeps the directory
  // up to date, this dialog closes
  if (!SynchronizeHooks().KeepUpToDateInNewWindow || FTerminal.isNull())
  {
    return;
  }
  SaveDirectoryHistory(FLocalDirectoryEdit);
  SaveDirectoryHistory(FRemoteDirectoryEdit);
  SynchronizeHooks().KeepUpToDateInNewWindow(FTerminal, Params(), CopyParams());
  QDialog::accept();
}

void SynchronizeDialog::MinimizeToTray()
{
  // TSynchronizeDialog::Minimize with MinimizeToTray: the application
  // windows hide, the tray icon restores them
  if (!QSystemTrayIcon::isSystemTrayAvailable())
  {
    MinimizeApplication(this);
    FMinimizedByMe = true;
    return;
  }
  if (FTrayIcon == nullptr)
  {
    FTrayIcon = new QSystemTrayIcon(this);
    QIcon Icon = QApplication::windowIcon();
    if (Icon.isNull())
    {
      Icon = style()->standardIcon(QStyle::SP_DriveNetIcon);
    }
    FTrayIcon->setIcon(Icon);
    auto * TrayMenu = new QMenu(this);
    QAction * RestoreAction = TrayMenu->addAction(QStringLiteral("&Restore"));
    connect(RestoreAction, &QAction::triggered, this, [this]() { RestoreFromTray(); });
    FTrayIcon->setContextMenu(TrayMenu);
    connect(FTrayIcon, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason Reason)
    {
      if ((Reason == QSystemTrayIcon::Trigger) || (Reason == QSystemTrayIcon::DoubleClick))
      {
        RestoreFromTray();
      }
    });
  }
  FTrayIcon->setToolTip(windowTitle());
  FHiddenWindows.clear();
  QWidget * Main = (parentWidget() != nullptr) ? parentWidget()->window() : nullptr;
  if (Main != nullptr)
  {
    FHiddenWindows.append(Main);
  }
  FHiddenWindows.append(this);
  FTrayIcon->show();
  for (const QPointer<QWidget> & Window : FHiddenWindows)
  {
    Window->hide();
  }
}

void SynchronizeDialog::RestoreFromTray()
{
  if ((FTrayIcon == nullptr) || !FTrayIcon->isVisible())
  {
    return;
  }
  FTrayIcon->hide();
  for (const QPointer<QWidget> & Window : FHiddenWindows)
  {
    if (!Window.isNull())
    {
      Window->showNormal();
    }
  }
  FHiddenWindows.clear();
  raise();
  activateWindow();
}

void SynchronizeDialog::TooManyDirectories(int & MaxDirectories, bool & Abort)
{
  // TCustomScpExplorerForm::DoSynchronizeTooManyDirectories
  int Configured = (FNewMaxWatchDirectories > 0) ? FNewMaxWatchDirectories : FMaxWatchDirectories;
  if (MaxDirectories < Configured)
  {
    MaxDirectories = Configured;
    return;
  }
  // TOO_MANY_WATCH_DIRECTORIES
  RestoreFromTray();
  bool NeverAsk = false;
  QMessageBox::StandardButton Answer = MessageWithNeverAsk(this, QMessageBox::Question,
    QStringLiteral("More than %1 directories and subdirectories found. Watching for changes in large number of directories "
      "can significantly degrade performance of the computer.").arg(MaxDirectories),
    QStringLiteral("Do you want to scan for another up to %1 directories?").arg(MaxDirectories),
    QMessageBox::Yes | QMessageBox::No, QStringLiteral("Never ask me a&gain"), NeverAsk);
  if (Answer == QMessageBox::Yes)
  {
    MaxDirectories *= 2;
    if (NeverAsk)
    {
      FNewMaxWatchDirectories = MaxDirectories;
    }
  }
  else
  {
    Abort = true;
  }
}

void SynchronizeDialog::UpdateControls()
{
  FStartButton->setEnabled(!FLocalDirectoryEdit->currentText().isEmpty() && !FRemoteDirectoryEdit->currentText().isEmpty());
  FStartContainer->setVisible(!FSynchronizing);
  FStopButton->setVisible(FSynchronizing);
  FStartButton->setDefault(!FSynchronizing);
  FStopButton->setDefault(FSynchronizing);
  // SYNCHRONIZE_SYCHRONIZING / SYNCHRONIZE_TITLE
  setWindowTitle(FSynchronizing ? QStringLiteral("Keeping remote directory up to date ...") : QStringLiteral("Keep remote directory up to date"));
  FTransferSettingsButton->setEnabled(!FSynchronizing);
  // Close and Minimize share the place
  FCancelButton->setVisible(!FSynchronizing);
  FCancelButton->setEnabled(!FSynchronizing);
  FMinimizeContainer->setVisible(FSynchronizing);
  FDirectoriesGroup->setEnabled(!FSynchronizing);
  FOptionsGroup->setEnabled(!FSynchronizing);
  FCopyParamGroup->setEnabled(!FSynchronizing);
  FSynchronizeSelectedOnlyCheck->setEnabled(FOptionsGroup->isEnabled() && ((FOptions & soAllowSelectedOnly) != 0));
  QString InfoStr = CopyParams().GetInfoStr(QStringLiteral("; "), ActualCopyParamAttrs());
  FCopyParamLabel->setText(InfoStr);
  FCopyParamLabel->setToolTip(InfoStr);
  FLogView->setVisible(FSynchronizing || (FLogView->topLevelItemCount() > 0));
}

void SynchronizeDialog::SetParams(const SynchronizeParamType & Value)
{
  FParams = Value;
  FRemoteDirectoryEdit->setEditText(Value.RemoteDirectory);
  FLocalDirectoryEdit->setEditText(Value.LocalDirectory);
  FSynchronizeDeleteCheck->setChecked((Value.Params & spDelete) != 0);
  FSynchronizeExistingOnlyCheck->setChecked((Value.Params & spExistingOnly) != 0);
  FSynchronizeSelectedOnlyCheck->setChecked((Value.Params & spSelectedOnly) != 0);
  FSynchronizeRecursiveCheck->setChecked((Value.Options & soRecurse) != 0);
  FSynchronizeSynchronizeCheck->setCheckState(
    ((Value.Options & soSynchronizeAsk) != 0) ? Qt::PartiallyChecked :
      (((Value.Options & soSynchronize) != 0) ? Qt::Checked : Qt::Unchecked));
  FContinueOnErrorCheck->setChecked((Value.Options & soContinueOnError) != 0);
  UpdateControls();
}

SynchronizeParamType SynchronizeDialog::Params() const
{
  SynchronizeParamType Result = FParams;
  Result.RemoteDirectory = FRemoteDirectoryEdit->currentText();
  Result.LocalDirectory = FLocalDirectoryEdit->currentText();
  Result.Params =
    (Result.Params & ~(spDelete | spExistingOnly | spSelectedOnly | spTimestamp)) |
    (FSynchronizeDeleteCheck->isChecked() ? spDelete : 0) |
    (FSynchronizeExistingOnlyCheck->isChecked() ? spExistingOnly : 0) |
    (FSynchronizeSelectedOnlyCheck->isChecked() ? spSelectedOnly : 0);
  Result.Options =
    (Result.Options & ~(soRecurse | soSynchronize | soSynchronizeAsk | soContinueOnError)) |
    (FSynchronizeRecursiveCheck->isChecked() ? soRecurse : 0) |
    ((FSynchronizeSynchronizeCheck->checkState() == Qt::Checked) ? soSynchronize : 0) |
    ((FSynchronizeSynchronizeCheck->checkState() == Qt::PartiallyChecked) ? soSynchronizeAsk : 0) |
    (FContinueOnErrorCheck->isChecked() ? soContinueOnError : 0);
  return Result;
}

CopyParam SynchronizeDialog::CopyParams() const
{
  CopyParam Result = FCopyParams;
  Result.PreserveTime = true;
  Result.NewerOnly = false;
  return Result;
}

void SynchronizeDialog::Start()
{
  bool Synchronize = false;
  if (FSynchronizeSynchronizeCheck->checkState() == Qt::PartiallyChecked)
  {
    // SYNCHRONISE_BEFORE_KEEPUPTODATE2
    bool NeverAsk = false;
    QMessageBox::StandardButton Answer = MessageWithNeverAsk(this, QMessageBox::Question,
      QStringLiteral("Do you want to perform full synchronisation of the remote directory first?"),
      QStringLiteral("Function 'Keep remote directory up to date' works correctly only, if the remote directory "
        "is synchronised with the local one before it starts."),
      QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QStringLiteral("Never ask me a&gain"), NeverAsk,
      QStringLiteral("task_keep_up_to_date"));   // HELP_KEEPUPTODATE_SYNCHRONIZE
    switch (Answer)
    {
      case QMessageBox::Yes:
        if (NeverAsk)
        {
          FSynchronizeSynchronizeCheck->setCheckState(Qt::Checked);
        }
        Synchronize = true;
        break;
      case QMessageBox::No:
        Synchronize = false;
        break;
      default:
        return;
    }
  }
  else
  {
    Synchronize = (FSynchronizeSynchronizeCheck->checkState() == Qt::Checked);
  }

  if (FTerminal.isNull() || !FTerminal->Active())
  {
    return;
  }

  SaveDirectoryHistory(FLocalDirectoryEdit);
  SaveDirectoryHistory(FRemoteDirectoryEdit);
  FSynchronizing = true;
  ClearLog();
  UpdateControls();

  // TSynchronizeDialog::DoStartStop
  SynchronizeParamType SParams = Params();
  SParams.Options = (SParams.Options & ~(soSynchronize | soSynchronizeAsk)) | (Synchronize ? soSynchronize : 0);
  SynchronizeOptions Options;
  if (((SParams.Params & spSelectedOnly) != 0) && ((FOptions & soAllowSelectedOnly) != 0))
  {
    // TCustomScpExplorerForm::GetSynchronizeOptions
    Options.UseFilter = true;
    Options.Filter = FSelectedNames;
  }
  FController.SetMaxWatchDirectories(FMaxWatchDirectories);
  FController.Start(SParams, CopyParams(), Options);
}

void SynchronizeDialog::Stop()
{
  FSynchronizing = false;
  FController.Stop();
  if (!FProgress.isNull())
  {
    FProgress->Finish();
    FProgress->deleteLater();
  }
  UpdateControls();
  // TSynchronizeDialog::Stop: restore the application minimized by "Minimize"
  RestoreFromTray();
  if (FMinimizedByMe)
  {
    FMinimizedByMe = false;
    QWidget * Main = (parentWidget() != nullptr) ? parentWidget()->window() : nullptr;
    if ((Main != nullptr) && Main->isMinimized())
    {
      Main->showNormal();
    }
    if (isMinimized())
    {
      showNormal();
    }
    raise();
    activateWindow();
  }
}

void SynchronizeDialog::DoLog(SynchronizeLogEntry Entry, const QString & Message, const QString & MoreMessages)
{
  auto * Item = new QTreeWidgetItem();
  Item->setText(0, QTime::currentTime().toString(QStringLiteral("HH:mm:ss")));
  QString Unformatted = Message;
  Unformatted.remove(QLatin1Char('\r'));
  Unformatted.replace(QLatin1Char('\n'), QLatin1Char(' '));
  Item->setText(1, Unformatted);
  Item->setData(0, LogEntryRole, static_cast<int>(Entry));
  Item->setData(0, LogMessageRole, Message);
  Item->setData(0, LogMoreRole, MoreMessages);
  if (Entry == slContinuedError)
  {
    Item->setForeground(0, Qt::red);
    Item->setForeground(1, Qt::red);
  }
  FLogView->addTopLevelItem(Item);
  while (FLogView->topLevelItemCount() > MaxLogItems)
  {
    delete FLogView->takeTopLevelItem(0);
  }
  FLogView->scrollToItem(Item);
  FLogView->setVisible(true);
}

void SynchronizeDialog::CopyLog()
{
  QString Content;
  for (int Index = 0; Index < FLogView->topLevelItemCount(); Index++)
  {
    QTreeWidgetItem * Item = FLogView->topLevelItem(Index);
    Content += Item->text(0) + QLatin1Char('\t') + Item->text(1) + QLatin1Char('\n');
  }
  QApplication::clipboard()->setText(Content);
}

bool SynchronizeDialog::eventFilter(QObject * Object, QEvent * Event)
{
  if ((Object == FLogView) && (Event->type() == QEvent::KeyPress))
  {
    auto * KeyEvent = static_cast<QKeyEvent *>(Event);
    if (KeyEvent->key() == Qt::Key_Delete)
    {
      ClearLog();
      return true;
    }
    else if (KeyEvent->matches(QKeySequence::Copy) && (FLogView->topLevelItemCount() > 0))
    {
      CopyLog();
      return true;
    }
  }
  return QDialog::eventFilter(Object, Event);
}

void SynchronizeDialog::keyPressEvent(QKeyEvent * Event)
{
  if ((Event->key() == Qt::Key_Escape) && FSynchronizing)
  {
    Stop();
    return;
  }
  QDialog::keyPressEvent(Event);
}

void SynchronizeDialog::reject()
{
  // TSynchronizeDialog::FormCloseQuery
  if (FSynchronizing)
  {
    Stop();
  }
  QDialog::reject();
}

}

//---------------------------------------------------------------------------
// Entry points

void RunFullSynchronize(QWidget * Parent, Terminal * Term, const QString & LocalDirectory,
  const QString & RemoteDirectory, bool LocalPanelActive, const CopyParam & Param,
  const QStringList & SelectedNames, std::function<void()> Finished)
{
  // TScpCommanderForm::FullSynchronizeDirectories + DoFullSynchronizeDirectories
  SynchronizeConfiguration Config = SynchronizeConfiguration::Load();
  bool SaveMode = (Config.ModeAuto >= 0);
  SynchronizeMode Mode = SaveMode ? static_cast<SynchronizeMode>(Config.ModeAuto) : (LocalPanelActive ? smRemote : smLocal);
  int Params = Config.Params;
  bool AllowSelectedOnly = !SelectedNames.isEmpty();
  int Options = (AllowSelectedOnly ? fsoAllowSelectedOnly : 0);

  FullSynchronizeDialog Dialog(Options, Parent);
  Dialog.SetMode(Mode);
  Dialog.SetParams(Params);
  Dialog.SetDirectory1(LocalDirectory);
  Dialog.SetDirectory2(RemoteDirectory);
  Dialog.SetCopyParams(Param);
  Dialog.SetSaveSettings(false);
  Dialog.SetSaveMode(SaveMode);
  if ((Dialog.exec() != QDialog::Accepted) || (Term == nullptr) || !Term->Active())
  {
    if (Finished)
    {
      Finished();
    }
    return;
  }

  auto Run = std::make_shared<FullSynchronizeRun>();
  Run->Parent = Parent;
  Run->Term = Term;
  Run->Mode = Dialog.Mode();
  Run->Params = Dialog.Params();
  Run->Directory1 = Dialog.Directory1();
  Run->Directory2 = Dialog.Directory2();
  Run->Param = Dialog.CopyParams();
  Run->Finished = std::move(Finished);
  // TerminalManager OnDeleteLocalFile: local deletes honour DeleteToRecycleBin
  Run->DeleteLocalToTrash = Configuration::Instance().DeleteToRecycleBin;
  if (((Run->Params & spSelectedOnly) != 0) && AllowSelectedOnly)
  {
    Run->Options.UseFilter = true;
    Run->Options.Filter = SelectedNames;
  }

  if (Dialog.SaveSettings())
  {
    Config.Params = Run->Params;
    if (Dialog.SaveMode())
    {
      Config.ModeAuto = Run->Mode;
    }
    Config.Save();
  }

  if (Dialog.StartInNewWindow() && SynchronizeHooks().FullSynchronizeInNewWindow)
  {
    // TCustomScpExplorerForm::FullSynchronizeInNewWindow
    SynchronizeHooks().FullSynchronizeInNewWindow(Term, Run->Mode, Run->Params, Run->Directory1, Run->Directory2, Run->Param);
    Run->Done();
    return;
  }

  Run->Collect();
}

void RunKeepRemoteUpToDate(QWidget * Parent, Terminal * Term, const QString & LocalDirectory,
  const QString & RemoteDirectory, const CopyParam & Param, const QStringList & SelectedNames,
  std::function<void()> Finished)
{
  // TCustomScpExplorerForm::DoSynchronizeDirectories
  SynchronizeConfiguration Config = SynchronizeConfiguration::Load();
  SynchronizeParamType Params;
  Params.LocalDirectory = LocalDirectory;
  Params.RemoteDirectory = RemoteDirectory;
  int UnusedParams =
    (Config.Params &
      (spPreviewChanges | spTimestamp | spNotByTime | spBySize | spCaseSensitive | spMirror | spByChecksum));
  Params.Params = Config.Params & ~UnusedParams;
  Params.Options = Config.Options;
  int Options = (!SelectedNames.isEmpty() ? soAllowSelectedOnly : 0);

  if ((Term != nullptr) && Term->Active())
  {
    SynchronizeDialog Dialog(Term, Options, Parent);
    Dialog.SetParams(Params);
    Dialog.SetCopyParams(Param);
    Dialog.SetSaveSettings(false);
    Dialog.SetSelectedNames(SelectedNames);
    Dialog.SetChangeDelay(Config.KeepUpToDateChangeDelay);
    Dialog.SetMaxWatchDirectories(Config.MaxWatchDirectories);
    Dialog.exec();
    SynchronizeParamType Result = Dialog.Params();
    if (Dialog.NewMaxWatchDirectories() > 0)
    {
      // "Never ask me again" of TOO_MANY_WATCH_DIRECTORIES
      Config.MaxWatchDirectories = Dialog.NewMaxWatchDirectories();
      Config.Save();
    }
    if (Dialog.SaveSettings())
    {
      Config.Params = Result.Params | UnusedParams;
      Config.Options = Result.Options;
      Config.Save();
    }
    else if (((Config.Options & soSynchronizeAsk) != 0) &&
             ((Result.Options & soSynchronizeAsk) == 0) &&
             ((Result.Options & soSynchronize) != 0))
    {
      // "Never ask me again" answered in the prompt
      Config.Options = (Config.Options & ~soSynchronizeAsk) | soSynchronize;
      Config.Save();
    }
  }
  if (Finished)
  {
    Finished();
  }
}

void RunCompareDirectories(QWidget * Parent, const QString & LocalDirectory, const RemoteFileList & RemoteFiles,
  std::function<void(const QStringList & LocalNames, const QStringList & RemoteNames)> Select)
{
  // TScpCommanderForm::CompareDirectories
  Configuration & Config = Configuration::Instance();
  QStringList LocalChanged;
  QStringList RemoteChanged;
  QApplication::setOverrideCursor(Qt::WaitCursor);
  CompareDirectories(LocalDirectory, RemoteFiles, Config.CompareByTime, Config.CompareBySize, LocalChanged, RemoteChanged);
  QApplication::restoreOverrideCursor();
  if (Select)
  {
    Select(LocalChanged, RemoteChanged);
  }
  if (LocalChanged.isEmpty() && RemoteChanged.isEmpty())
  {
    // COMPARE_NO_DIFFERENCES, HELP_COMPARE_NO_DIFFERENCES
    MessageDialog(Parent, QStringLiteral("No differences found."), qtInformation, qaOK, QStringLiteral("task_compare_directories"));
  }
}
