// Port of WinSCP source/forms/Console.{cpp,dfm}
// (window size persisted as Configuration\Interface\ConsoleWin\WindowSize,
// "Width,Height,PixelsPerInch" as RestoreFormSize/StoreFormSize; works for every
// protocol with AnyCommand: shell commands for SCP/SFTP, raw protocol commands for FTP)
#include "gui/ConsoleDialog.h"
#include "gui/HelpSystem.h"
#include "gui/HistoryComboBox.h"

#include "core/Configuration.h"
#include "core/IniStorage.h"
#include "core/Terminal.h"

#include <QAction>
#include <QCloseEvent>
#include <QComboBox>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QScrollBar>
#include <QStyle>
#include <QTextBlock>
#include <QVBoxLayout>

namespace
{

const QString CommandsHistoryKey = QStringLiteral("Commands");

// TPathLabel with UnixPath: single line, eliding the middle of the path
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
    FPath = Path;
    setToolTip(Path);
    UpdateText();
  }

protected:
  void resizeEvent(QResizeEvent * Event) override
  {
    QLabel::resizeEvent(Event);
    UpdateText();
  }

private:
  QString FPath;

  void UpdateText()
  {
    setText(fontMetrics().elidedText(FPath, Qt::ElideMiddle, std::max(width(), 10)));
  }
};

}

struct ConsoleDialog::Private
{
  QPointer<Terminal> Term;
  QComboBox * CommandEdit = nullptr;
  QPushButton * ExecuteButton = nullptr;
  QPushButton * CancelBtn = nullptr;
  PathLabel * DirectoryLabel = nullptr;
  QPlainTextEdit * OutputMemo = nullptr;
  QAction * AdjustWindowAction = nullptr;
  bool Executing = false;
  bool DirectoryChanged = false;
  QString StartDirectory;
  QStringList ErrorLines;
};

ConsoleDialog::ConsoleDialog(Terminal * ATerminal, QWidget * Parent) :
  QDialog(Parent),
  d(new Private)
{
  d->Term = ATerminal;
  d->StartDirectory = ATerminal->CurrentDirectory();
  setWindowTitle(QStringLiteral("Console"));
  setWindowFlags(windowFlags() | Qt::WindowMinMaxButtonsHint);
  setMinimumSize(420, 250);
  resize(551, 393);

  auto * Image = new QLabel();
  QIcon Icon = QIcon::fromTheme(QStringLiteral("utilities-terminal"), style()->standardIcon(QStyle::SP_ComputerIcon));
  Image->setPixmap(Icon.pixmap(32, 32));
  Image->setAlignment(Qt::AlignTop | Qt::AlignHCenter);

  auto * Label1 = new QLabel(QStringLiteral("Enter &command:"));
  d->CommandEdit = new QComboBox();
  d->CommandEdit->setEditable(true);
  d->CommandEdit->setInsertPolicy(QComboBox::NoInsert);
  d->CommandEdit->lineEdit()->setMaxLength(2048);
  d->CommandEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  HistoryComboBox::LoadHistoryCombo(d->CommandEdit, CommandsHistoryKey);
  d->CommandEdit->setCurrentText(QString());
  Label1->setBuddy(d->CommandEdit);

  auto * Label4 = new QLabel(QStringLiteral("Do not execute commands that require user-input or data transfer"));
  Label4->setTextFormat(Qt::PlainText);
  auto * Label2 = new QLabel(QStringLiteral("Current directory:"));
  Label2->setTextFormat(Qt::PlainText);
  d->DirectoryLabel = new PathLabel();

  d->ExecuteButton = new QPushButton(QStringLiteral("&Execute"));
  d->ExecuteButton->setDefault(true);
  d->CancelBtn = new QPushButton(QStringLiteral("Close"));
  d->CancelBtn->setAutoDefault(false);
  auto * HelpButton = new QPushButton(QStringLiteral("&Help"));
  HelpButton->setAutoDefault(false);

  auto * Top = new QGridLayout();
  Top->addWidget(Image, 0, 0, 3, 1);
  Top->addWidget(Label1, 0, 1);
  Top->addWidget(d->CommandEdit, 0, 2);
  Top->addWidget(d->ExecuteButton, 0, 3);
  Top->addWidget(d->CancelBtn, 0, 4);
  Top->addWidget(Label4, 1, 1, 1, 3);
  Top->addWidget(HelpButton, 1, 4, 2, 1, Qt::AlignTop);
  Top->addWidget(Label2, 2, 1);
  Top->addWidget(d->DirectoryLabel, 2, 2, 1, 2);
  Top->setColumnStretch(2, 1);
  Top->setContentsMargins(11, 8, 8, 8);

  auto * Bevel = new QFrame();
  Bevel->setFrameShape(QFrame::HLine);
  Bevel->setFrameShadow(QFrame::Sunken);

  d->OutputMemo = new QPlainTextEdit();
  d->OutputMemo->setReadOnly(true);
  d->OutputMemo->setFocusPolicy(Qt::ClickFocus);
  d->OutputMemo->setLineWrapMode(QPlainTextEdit::NoWrap);
  d->OutputMemo->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  QPalette Palette = d->OutputMemo->palette();
  // clBlack background, 0x00BBBBBB text
  Palette.setColor(QPalette::Base, Qt::black);
  Palette.setColor(QPalette::Text, QColor(0xBB, 0xBB, 0xBB));
  d->OutputMemo->setPalette(Palette);
  d->OutputMemo->setContextMenuPolicy(Qt::CustomContextMenu);

  auto * Layout = new QVBoxLayout(this);
  Layout->setContentsMargins(0, 0, 0, 0);
  Layout->setSpacing(0);
  Layout->addLayout(Top);
  Layout->addWidget(Bevel);
  Layout->addWidget(d->OutputMemo, 1);

  // Popup menu: Copy, Select All, -, Adjust Window
  auto * CopyAction = new QAction(QStringLiteral("&Copy"), this);
  CopyAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_C));
  CopyAction->setShortcutContext(Qt::WidgetShortcut);
  connect(CopyAction, &QAction::triggered, d->OutputMemo, &QPlainTextEdit::copy);
  auto * SelectAllAction = new QAction(QStringLiteral("Select &All"), this);
  SelectAllAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_A));
  SelectAllAction->setShortcutContext(Qt::WidgetShortcut);
  connect(SelectAllAction, &QAction::triggered, d->OutputMemo, &QPlainTextEdit::selectAll);
  d->AdjustWindowAction = new QAction(QStringLiteral("Adjust &Window"), this);
  d->AdjustWindowAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_J));
  d->AdjustWindowAction->setShortcutContext(Qt::WindowShortcut);
  addAction(d->AdjustWindowAction);
  d->OutputMemo->addAction(CopyAction);
  d->OutputMemo->addAction(SelectAllAction);
  connect(d->OutputMemo, &QWidget::customContextMenuRequested, this, [this, CopyAction, SelectAllAction](const QPoint & Pos)
  {
    QMenu Menu(this);
    CopyAction->setEnabled(d->OutputMemo->textCursor().hasSelection());
    Menu.addAction(CopyAction);
    Menu.addAction(SelectAllAction);
    Menu.addSeparator();
    Menu.addAction(d->AdjustWindowAction);
    Menu.exec(d->OutputMemo->viewport()->mapToGlobal(Pos));
  });

  // TConsoleDialog::DoAdjustWindow: resize to fit the output
  connect(d->AdjustWindowAction, &QAction::triggered, this, [this]()
  {
    QFontMetrics Metrics(d->OutputMemo->font());
    int Columns = 0;
    int Rows = d->OutputMemo->blockCount();
    for (QTextBlock Block = d->OutputMemo->document()->begin(); Block.isValid(); Block = Block.next())
    {
      Columns = std::max(Columns, Metrics.horizontalAdvance(Block.text()));
    }
    // 10 is surplus to cover any borders, etc.; there is always one line more
    int RequiredWidth = Columns + 10 + d->OutputMemo->verticalScrollBar()->sizeHint().width();
    int RequiredHeight = Metrics.lineSpacing() * (Rows + 1) + 10;
    QSize Viewport = d->OutputMemo->viewport()->size();
    QSize NewSize(width() + (RequiredWidth - Viewport.width()), height() + (RequiredHeight - Viewport.height()));
    if (QScreen * Screen = screen())
    {
      NewSize = NewSize.boundedTo(Screen->availableSize());
    }
    resize(NewSize.expandedTo(minimumSize()));
  });

  connect(d->ExecuteButton, &QPushButton::clicked, this, [this]()
  {
    // TConsoleDialog::ExecuteButtonClick
    QString Command = d->CommandEdit->currentText();
    if (Command.trimmed().isEmpty() || d->Executing || !d->Term)
    {
      return;
    }
    HistoryComboBox::SaveToHistory(CommandsHistoryKey, Command);
    HistoryComboBox::LoadHistoryCombo(d->CommandEdit, CommandsHistoryKey);
    d->CommandEdit->setCurrentText(Command);
    d->CommandEdit->lineEdit()->selectAll();

    QString CurrentDirectory = d->Term->CurrentDirectory();
    d->OutputMemo->appendPlainText(QStringLiteral("%1$ %2").arg(CurrentDirectory, Command));
    d->Executing = true;
    d->ErrorLines.clear();
    // UpdateControls
    d->ExecuteButton->setEnabled(false);
    d->CancelBtn->setEnabled(false);
    QPointer<ConsoleDialog> Self(this);
    d->Term->AnyCommand(Command,
      [Self](const QString & Line, bool StdError)
      {
        if (Self)
        {
          Self->d->OutputMemo->appendPlainText(Line);
          if (StdError)
          {
            Self->d->ErrorLines.append(Line);
          }
        }
      },
      [Self, Command](const OperationResult & Result, int ExitCode, const QString &)
      {
        if (!Self)
        {
          return;
        }
        Private * D = Self->d;
        D->Executing = false;
        if (D->Term)
        {
          D->DirectoryChanged = (D->Term->CurrentDirectory() != D->StartDirectory);
        }
        D->DirectoryLabel->SetPath(D->Term ? D->Term->CurrentDirectory() : QString());
        D->ExecuteButton->setEnabled(!D->CommandEdit->currentText().trimmed().isEmpty() && D->Term && D->Term->Active());
        D->CancelBtn->setEnabled(true);
        emit Self->CommandExecuted();
        QString Message;
        QString More;
        if (!Result.Ok)
        {
          if (Result.Aborted)
          {
            return;
          }
          Message = Result.Message;
          More = Result.MoreMessages;
        }
        else
        {
          // TSCPFileSystem::AnyCommand: exit code 1 is a warning, unless ExitCode1IsError
          bool Error = (ExitCode > 1) || ((ExitCode == 1) && D->Term && D->Term->Data().ExitCode1IsError);
          if (Error)
          {
            if (D->ErrorLines.isEmpty())
            {
              Message = QStringLiteral("Command failed with return code %1.").arg(ExitCode);
            }
            else
            {
              Message = QStringLiteral("Command '%1'\nfailed with return code %2 and the following error message.").arg(Command).arg(ExitCode);
              More = D->ErrorLines.join(QLatin1Char('\n'));
            }
          }
        }
        if (!Message.isEmpty())
        {
          QMessageBox Box(QMessageBox::Critical, QStringLiteral("Error"), Message, QMessageBox::Ok, Self);
          Box.setDetailedText(More);
          if (!More.isEmpty() && (More.size() < 400))
          {
            Box.setInformativeText(More);
            Box.setDetailedText(QString());
          }
          Box.exec();
        }
        if (Self && !Result.Fatal)
        {
          Self->d->CommandEdit->setFocus();
        }
      });
  });

  connect(d->CommandEdit, &QComboBox::editTextChanged, this, [this](const QString & Text)
  {
    d->ExecuteButton->setEnabled(!Text.trimmed().isEmpty() && !d->Executing && d->Term && d->Term->Active());
  });
  connect(d->CancelBtn, &QPushButton::clicked, this, &QDialog::reject);
  HelpSystem::InstallFormHelp(this, QStringLiteral("ui_console"), HelpButton);
  // TConsoleDialog::TerminalClose
  connect(ATerminal, &Terminal::Disconnected, this, [this]()
  {
    d->Executing = false;
    reject();
  });
  connect(ATerminal, &QObject::destroyed, this, [this]()
  {
    d->Executing = false;
    reject();
  });

  d->DirectoryLabel->SetPath(ATerminal->CurrentDirectory());
  d->ExecuteButton->setEnabled(false);
  d->CommandEdit->setFocus();
}

ConsoleDialog::~ConsoleDialog()
{
  delete d;
}

bool ConsoleDialog::DirectoryChanged() const
{
  return d->DirectoryChanged;
}

bool ConsoleDialog::Executing() const
{
  return d->Executing;
}

void ConsoleDialog::closeEvent(QCloseEvent * Event)
{
  // TConsoleDialog::FormCloseQuery
  if (d->Executing)
  {
    Event->ignore();
    return;
  }
  QDialog::closeEvent(Event);
}

void ConsoleDialog::reject()
{
  if (d->Executing)
  {
    return;
  }
  QDialog::reject();
}

// TCustomWinConfiguration ConsoleWin.WindowSize, default FormatDefaultWindowSize(570, 430)
static QString ConsoleWindowSize()
{
  IniStorage Storage(&Configuration::Instance().Ini());
  QString Result = QStringLiteral("570,430,96");
  if (Storage.OpenSubKeyPath(QStringLiteral("Configuration\\Interface\\ConsoleWin"), false))
  {
    Result = Storage.ReadString(QStringLiteral("WindowSize"), Result);
  }
  return Result;
}

static void SetConsoleWindowSize(const QString & Value)
{
  IniStorage Storage(&Configuration::Instance().Ini());
  if (Storage.OpenSubKeyPath(QStringLiteral("Configuration\\Interface\\ConsoleWin"), true))
  {
    Storage.WriteString(QStringLiteral("WindowSize"), Value);
  }
}

// RestoreFormSize: "Width,Height,PixelsPerInch"
static void RestoreFormSize(QWidget * Form, const QString & Data)
{
  QStringList Parts = Data.split(QLatin1Char(','));
  bool WidthOk = false;
  bool HeightOk = false;
  int Width = Parts.value(0).trimmed().toInt(&WidthOk);
  int Height = Parts.value(1).trimmed().toInt(&HeightOk);
  int PixelsPerInch = Parts.value(2).trimmed().toInt();
  if (PixelsPerInch <= 0)
  {
    PixelsPerInch = 96;
  }
  int Dpi = (Form->logicalDpiX() > 0) ? Form->logicalDpiX() : 96;
  if (WidthOk && HeightOk && (Width > 0) && (Height > 0))
  {
    QSize Size(Width * Dpi / PixelsPerInch, Height * Dpi / PixelsPerInch);
    if (QScreen * Screen = Form->screen())
    {
      Size = Size.boundedTo(Screen->availableSize());
    }
    Form->resize(Size.expandedTo(Form->minimumSize()));
  }
}

// StoreFormSize
static QString StoreFormSize(QWidget * Form)
{
  int Dpi = (Form->logicalDpiX() > 0) ? Form->logicalDpiX() : 96;
  return QStringLiteral("%1,%2,%3").arg(Form->width()).arg(Form->height()).arg(Dpi);
}

bool ConsoleDialog::Execute(Terminal * ATerminal, QWidget * Parent, const QString & Command)
{
  ConsoleDialog Dialog(ATerminal, Parent);
  // FormShow: RestoreFormSize(ConsoleWin.WindowSize), FAutoBounds
  RestoreFormSize(&Dialog, ConsoleWindowSize());
  QSize AutoSize = Dialog.size();
  if (!Command.isEmpty())
  {
    Dialog.d->CommandEdit->setCurrentText(Command);
    QMetaObject::invokeMethod(Dialog.d->ExecuteButton, &QPushButton::click, Qt::QueuedConnection);
  }
  Dialog.exec();
  // stored only when the user changed the size
  if (Dialog.size() != AutoSize)
  {
    SetConsoleWindowSize(StoreFormSize(&Dialog));
  }
  return Dialog.DirectoryChanged();
}
