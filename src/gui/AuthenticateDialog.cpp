// Port of WinSCP source/forms/Authenticate.{cpp,dfm} (TAuthenticateForm)
#include "gui/AuthenticateDialog.h"
#include "gui/HelpSystem.h"

#include "gui/GuiUserInterface.h"
#include "gui/Icons.h"

#include <algorithm>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QEventLoop>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

// TitleSeparator (windows/GUITools.h)
static const QString TitleSeparator = QStringLiteral(" - ");

AuthenticateDialog::AuthenticateDialog(QWidget * Parent) :
  QDialog(Parent)
{
  setObjectName(QStringLiteral("AuthenticateForm"));
  setMinimumSize(280, 200);
  setSizeGripEnabled(true);
  auto * Layout = new QVBoxLayout(this);
  Layout->setContentsMargins(0, 0, 0, 0);
  Layout->setSpacing(0);

  // TopPanel: LeftPanel with AnimationPaintBox + LogView
  FTopPanel = new QWidget(this);
  FTopPanel->setObjectName(QStringLiteral("TopPanel"));
  auto * TopLayout = new QHBoxLayout(FTopPanel);
  TopLayout->setContentsMargins(8, 8, 8, 8);
  TopLayout->setSpacing(8);
  FAnimation = new FrameAnimation(FTopPanel);
  FAnimation->setObjectName(QStringLiteral("AnimationPaintBox"));
  TopLayout->addWidget(FAnimation, 0, Qt::AlignTop);
  FLogView = new QListWidget(FTopPanel);
  FLogView->setObjectName(QStringLiteral("LogView"));
  FLogView->setSelectionMode(QAbstractItemView::NoSelection);
  FLogView->setFocusPolicy(Qt::NoFocus);
  FLogView->setWordWrap(true);
  FLogView->setFrameShape(QFrame::NoFrame);
  FLogView->setMinimumHeight(63);
  FLogView->setMouseTracking(true);
  QPalette LogPalette = FLogView->palette();
  LogPalette.setColor(QPalette::Base, palette().color(QPalette::Window));
  FLogView->setPalette(LogPalette);
  TopLayout->addWidget(FLogView, 1);
  Layout->addWidget(FTopPanel, 1);

  // PasswordPanel
  FPasswordPanel = new QWidget(this);
  FPasswordPanel->setObjectName(QStringLiteral("PasswordPanel"));
  auto * PasswordLayout = new QVBoxLayout(FPasswordPanel);
  PasswordLayout->setContentsMargins(8, 8, 8, 8);
  FPromptEditPanel = new QWidget(FPasswordPanel);
  FPromptEditPanel->setObjectName(QStringLiteral("PromptEditPanel"));
  FPromptLayout = new QVBoxLayout(FPromptEditPanel);
  FPromptLayout->setContentsMargins(0, 0, 0, 0);
  PasswordLayout->addWidget(FPromptEditPanel);

  FSavePasswordPanel = new QWidget(FPasswordPanel);
  auto * SaveLayout = new QHBoxLayout(FSavePasswordPanel);
  SaveLayout->setContentsMargins(0, 0, 0, 0);
  FSavePasswordCheck = new QCheckBox(QStringLiteral("&Change stored password to this one"), FSavePasswordPanel);
  FSavePasswordCheck->setObjectName(QStringLiteral("SavePasswordCheck"));
  SaveLayout->addWidget(FSavePasswordCheck);
  PasswordLayout->addWidget(FSavePasswordPanel);

  FSessionRememberPasswordPanel = new QWidget(FPasswordPanel);
  auto * RememberLayout = new QHBoxLayout(FSessionRememberPasswordPanel);
  RememberLayout->setContentsMargins(0, 0, 0, 0);
  FSessionRememberPasswordCheck = new QCheckBox(QStringLiteral("&Remember password for this session"), FSessionRememberPasswordPanel);
  FSessionRememberPasswordCheck->setObjectName(QStringLiteral("SessionRememberPasswordCheck"));
  RememberLayout->addWidget(FSessionRememberPasswordCheck);
  PasswordLayout->addWidget(FSessionRememberPasswordPanel);

  auto * PasswordButtons = new QHBoxLayout();
  PasswordButtons->addStretch(1);
  FPasswordOKButton = new QPushButton(QStringLiteral("OK"), FPasswordPanel);
  FPasswordOKButton->setObjectName(QStringLiteral("PasswordOKButton"));
  FPasswordCancelButton = new QPushButton(QStringLiteral("Cancel"), FPasswordPanel);
  FPasswordCancelButton->setObjectName(QStringLiteral("PasswordCancelButton"));
  auto * PasswordHelpButton = new QPushButton(QStringLiteral("&Help"), FPasswordPanel);
  PasswordHelpButton->setObjectName(QStringLiteral("PasswordHelpButton"));
  for (QPushButton * Button : {FPasswordOKButton, FPasswordCancelButton, PasswordHelpButton})
  {
    Button->setMinimumWidth(80);
    PasswordButtons->addWidget(Button);
  }
  PasswordLayout->addLayout(PasswordButtons);
  connect(FPasswordOKButton, &QPushButton::clicked, this, [this]() { Finish(true); });
  connect(FPasswordCancelButton, &QPushButton::clicked, this, [this]() { Finish(false); });
  HelpSystem::InstallFormHelp(this, QStringLiteral("ui_authenticate"), PasswordHelpButton);
  Layout->addWidget(FPasswordPanel);
  FPasswordPanel->hide();

  // BannerPanel
  FBannerPanel = new QWidget(this);
  FBannerPanel->setObjectName(QStringLiteral("BannerPanel"));
  auto * BannerLayout = new QVBoxLayout(FBannerPanel);
  BannerLayout->setContentsMargins(8, 8, 8, 8);
  FBannerMemo = new QPlainTextEdit(FBannerPanel);
  FBannerMemo->setObjectName(QStringLiteral("BannerMemo"));
  FBannerMemo->setReadOnly(true);
  FBannerMemo->setLineWrapMode(QPlainTextEdit::NoWrap);
  FBannerMemo->setMinimumHeight(35);
  FBannerMemo->setContextMenuPolicy(Qt::CustomContextMenu);
  BannerLayout->addWidget(FBannerMemo, 1);
  FBannerMonospacedFontAction = new QAction(QStringLiteral("Use &Monospaced Font"), this);
  FBannerMonospacedFontAction->setCheckable(true);
  connect(FBannerMonospacedFontAction, &QAction::triggered, this, [this]() { UpdateBannerFont(); });
  connect(FBannerMemo, &QWidget::customContextMenuRequested, this, [this](const QPoint & Pos)
  {
    // BannerPopupMenu: Copy, Select All, -, Use Monospaced Font
    QMenu Menu(this);
    QAction * Copy = Menu.addAction(QStringLiteral("&Copy"));
    Copy->setEnabled(FBannerMemo->textCursor().hasSelection());
    connect(Copy, &QAction::triggered, FBannerMemo, &QPlainTextEdit::copy);
    QAction * SelectAll = Menu.addAction(QStringLiteral("Select &All"));
    connect(SelectAll, &QAction::triggered, FBannerMemo, &QPlainTextEdit::selectAll);
    Menu.addSeparator();
    Menu.addAction(FBannerMonospacedFontAction);
    Menu.exec(FBannerMemo->viewport()->mapToGlobal(Pos));
  });
  auto * BannerButtons = new QHBoxLayout();
  FNeverShowAgainCheck = new QCheckBox(QStringLiteral("&Never show this banner again"), FBannerPanel);
  FNeverShowAgainCheck->setObjectName(QStringLiteral("NeverShowAgainCheck"));
  BannerButtons->addWidget(FNeverShowAgainCheck);
  BannerButtons->addStretch(1);
  FBannerCloseButton = new QPushButton(QStringLiteral("Continue"), FBannerPanel);
  FBannerCloseButton->setObjectName(QStringLiteral("BannerCloseButton"));
  auto * BannerHelpButton = new QPushButton(QStringLiteral("&Help"), FBannerPanel);
  BannerHelpButton->setObjectName(QStringLiteral("BannerHelpButton"));
  for (QPushButton * Button : {FBannerCloseButton, BannerHelpButton})
  {
    Button->setMinimumWidth(80);
    BannerButtons->addWidget(Button);
  }
  BannerLayout->addLayout(BannerButtons);
  connect(FBannerCloseButton, &QPushButton::clicked, this, [this]() { Finish(true); });
  HelpSystem::InstallFormHelp(this, QStringLiteral("ui_authenticate"), BannerHelpButton);
  Layout->addWidget(FBannerPanel, 1);
  FBannerPanel->hide();

  resize(416, 270);
}

AuthenticateDialog::~AuthenticateDialog()
{
  if (FLoop != nullptr)
  {
    FLoop->quit();
  }
}

void AuthenticateDialog::Init(const QString & SessionName, bool Secure)
{
  FSessionName = SessionName;
  FAnimation->Init(Secure ? QStringLiteral("ConnectingSecure") : QStringLiteral("ConnectingInsecure"));
  AdjustControls();
}

void AuthenticateDialog::ShowAsModal()
{
  setWindowModality(Qt::ApplicationModal);
  show();
  FAnimation->Start();
}

void AuthenticateDialog::Log(const QString & Message, const QString & Additional)
{
  auto * Item = new QListWidgetItem(Message, FLogView);
  if (!Additional.isEmpty())
  {
    Item->setToolTip(Additional);
  }
  FHints.append(Additional);
  FLogView->scrollToItem(Item);
}

QStringList AuthenticateDialog::LogLines() const
{
  QStringList Result;
  for (int Index = 0; Index < FLogView->count(); Index++)
  {
    Result.append(FLogView->item(Index)->text());
  }
  return Result;
}

void AuthenticateDialog::AdjustControls()
{
  QString ACaption = FStatus.isEmpty() ? FSessionName : (FStatus + TitleSeparator + FSessionName);
  setWindowTitle(ACaption);
}

QLabel * AuthenticateDialog::GenerateLabel(const QString & Caption)
{
  auto * Result = new QLabel(Caption, FPromptEditPanel);
  Result->setWordWrap(true);
  Result->setTextFormat(Qt::PlainText);
  FPromptLayout->addWidget(Result);
  return Result;
}

bool AuthenticateDialog::ExtractUrl(const QString & Text, QString & Url)
{
  bool Result = false;
  QString Prefix = QStringLiteral("https://");
  int P = Text.indexOf(Prefix);
  if (P < 0)
  {
    Prefix = QStringLiteral("http://");
    P = Text.indexOf(Prefix);
  }
  if (P >= 0)
  {
    Url = Text.mid(P);
    for (int Index = 0; Index < Url.size(); Index++)
    {
      if (Url.at(Index).isSpace())
      {
        Url.truncate(Index);
        break;
      }
    }
    // At least one letter/digit after the prefix
    if ((Url.size() > Prefix.size()) && Url.at(Prefix.size()).isLetterOrNumber())
    {
      Result = true;
    }
  }
  return Result;
}

void AuthenticateDialog::ExternalLabel(QLabel * Label)
{
  // LabelPopupMenu: Copy, -, Open Link; a label with a URL opens it on click
  Label->setContextMenuPolicy(Qt::CustomContextMenu);
  QString Url;
  bool HasUrl = ExtractUrl(Label->text(), Url);
  connect(Label, &QWidget::customContextMenuRequested, this, [this, Label, HasUrl, Url](const QPoint & Pos)
  {
    QMenu Menu(this);
    QAction * Copy = Menu.addAction(QStringLiteral("&Copy"));
    connect(Copy, &QAction::triggered, this, [Label]() { QApplication::clipboard()->setText(Label->text()); });
    if (HasUrl)
    {
      Menu.addSeparator();
      QAction * Open = Menu.addAction(QStringLiteral("&Open Link"));
      connect(Open, &QAction::triggered, this, [Url]() { QDesktopServices::openUrl(QUrl(Url)); });
    }
    Menu.exec(Label->mapToGlobal(Pos));
  });
  if (HasUrl)
  {
    Label->setCursor(Qt::PointingHandCursor);
    Label->installEventFilter(this);
    Label->setProperty("NixSCPUrl", Url);
  }
}

void AuthenticateDialog::GeneratePrompt(UserInterface::PromptKind Kind, const QString & Instructions, const QList<Prompt> & Prompts)
{
  // DeleteChildren(FPromptParent)
  while (QLayoutItem * Item = FPromptLayout->takeAt(0))
  {
    if (QWidget * Widget = Item->widget())
    {
      Widget->deleteLater();
      Widget->hide();
    }
    delete Item;
  }
  FEdits.clear();

  if (!Instructions.trimmed().isEmpty())
  {
    QLabel * Label = GenerateLabel(Instructions.trimmed());
    Label->setObjectName(QStringLiteral("InstructionsLabel"));
    ExternalLabel(Label);
    FPromptLayout->addSpacing(4);
  }

  for (int Index = 0; Index < Prompts.size(); Index++)
  {
    QLabel * Label = GenerateLabel(Prompts[Index].Text.trimmed());
    Label->setObjectName(QStringLiteral("PromptLabel%1").arg(Index + 1));
    if (Kind == UserInterface::pkKeybInteractive)
    {
      ExternalLabel(Label);
    }
    else
    {
      // keep the accelerator (&Username:) working
      Label->setTextFormat(Qt::AutoText);
    }
    auto * Edit = new QLineEdit(FPromptEditPanel);
    Edit->setObjectName(QStringLiteral("PromptEdit%1").arg(Index + 1));
    Edit->setEchoMode(Prompts[Index].Echo ? QLineEdit::Normal : QLineEdit::Password);
    Edit->setText(Prompts[Index].Result);
    Edit->setMinimumWidth(280);
    Label->setBuddy(Edit);
    FPromptLayout->addWidget(Edit);
    FEdits.append(Edit);
  }
}

bool AuthenticateDialog::eventFilter(QObject * Watched, QEvent * Event)
{
  if ((Event->type() == QEvent::MouseButtonRelease) && (static_cast<QMouseEvent *>(Event)->button() == Qt::LeftButton))
  {
    QString Url = Watched->property("NixSCPUrl").toString();
    if (!Url.isEmpty())
    {
      // TAuthenticateForm::LinkClick
      QDesktopServices::openUrl(QUrl(Url));
      return true;
    }
  }
  return QDialog::eventFilter(Watched, Event);
}

bool AuthenticateDialog::PromptUser(UserInterface::PromptKind Kind, const QString & Title, const QString & Instructions,
  QList<Prompt> & Prompts, PromptOptions & Options)
{
  GeneratePrompt(Kind, Instructions, Prompts);

  FSavePasswordCheck->setChecked(false);
  FSavePasswordPanel->setVisible(Options.ShowSavePassword);
  FSessionRememberPasswordCheck->setChecked(false);
  // do not offer to remember password, if we are offering to save the password to stored site
  FSessionRememberPasswordPanel->setVisible(!Options.ShowSavePassword && Options.ShowSessionRememberPassword);

  QWidget * Focus = FEdits.isEmpty() ? static_cast<QWidget *>(FPasswordOKButton) : static_cast<QWidget *>(FEdits.first());
  bool Result = Execute(Title, FPasswordPanel, Focus, FPasswordOKButton, true, false, false);
  if (Result)
  {
    for (int Index = 0; Index < FEdits.size() && Index < Prompts.size(); Index++)
    {
      Prompts[Index].Result = FEdits[Index]->text();
    }
    Options.SavePassword = FSavePasswordPanel->isVisibleTo(FPasswordPanel) && FSavePasswordCheck->isChecked();
    Options.SessionRememberPassword =
      FSessionRememberPasswordPanel->isVisibleTo(FPasswordPanel) && FSessionRememberPasswordCheck->isChecked();
  }
  // Do not keep the typed secrets in the hidden edits
  for (QLineEdit * Edit : FEdits)
  {
    Edit->clear();
  }
  return Result;
}

void AuthenticateDialog::UpdateBannerFont()
{
  if (FBannerMonospacedFontAction->isChecked())
  {
    FBannerMemo->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  }
  else
  {
    FBannerMemo->setFont(font());
  }
}

void AuthenticateDialog::Banner(const QString & BannerText, bool & NeverShowAgain, int Options, unsigned int & Params)
{
  FBannerMemo->setPlainText(BannerText);
  FNeverShowAgainCheck->setVisible((Options & boDisableNeverShowAgain) == 0);
  FNeverShowAgainCheck->setChecked(NeverShowAgain);
  FBannerMonospacedFontAction->setChecked((Params & bpMonospacedFont) != 0);
  UpdateBannerFont();

  // AUTHENTICATION_BANNER
  bool Result = Execute(QStringLiteral("Authentication Banner"), FBannerPanel, FBannerCloseButton,
    FBannerCloseButton, false, true, false);
  if (Result)
  {
    NeverShowAgain = FNeverShowAgainCheck->isVisibleTo(FBannerPanel) && FNeverShowAgainCheck->isChecked();
    Params = (Params & ~bpMonospacedFont) | (FBannerMonospacedFontAction->isChecked() ? bpMonospacedFont : 0u);
  }
}

bool AuthenticateDialog::Execute(const QString & Status, QWidget * Panel, QWidget * FocusControl, QPushButton * DefaultButton,
  bool FixHeight, bool Zoom, bool ForceLog)
{
  bool WasVisible = isVisible();
  FAnimation->Stop();
  FStatus = Status;
  DefaultButton->setDefault(true);
  QSize PrevSize = size();
  QSize PrevMinimum = minimumSize();
  QSize PrevMaximum = maximumSize();

  bool ShowLog = (ForceLog || WasVisible) && !Zoom;
  FTopPanel->setVisible(ShowLog);
  Panel->show();
  if (!WasVisible)
  {
    // Only the panel (TAuthenticateForm::Execute when not Visible)
    adjustSize();
    if (Zoom)
    {
      resize(std::max(width(), 560), std::max(height(), 300));
    }
    else if (FixHeight)
    {
      int H = sizeHint().height();
      setMinimumHeight(H);
      setMaximumHeight(H);
      resize(std::max(width(), 416), H);
    }
    setWindowModality(Qt::ApplicationModal);
    show();
  }
  else if (height() < sizeHint().height())
  {
    resize(width(), sizeHint().height());
  }
  AdjustControls();
  raise();
  activateWindow();
  FocusControl->setFocus();

  QEventLoop Loop;
  FLoop = &Loop;
  FResult = false;
  FExecuting = true;
  Loop.exec(QEventLoop::DialogExec);
  FExecuting = false;
  FLoop = nullptr;

  Panel->hide();
  FTopPanel->show();
  DefaultButton->setDefault(false);
  FStatus.clear();
  AdjustControls();
  if (!WasVisible)
  {
    hide();
    setMinimumSize(PrevMinimum);
    setMaximumSize(PrevMaximum);
    resize(PrevSize);
  }
  else
  {
    FAnimation->Start();
  }
  return FResult;
}

void AuthenticateDialog::Finish(bool Ok)
{
  if (FLoop != nullptr)
  {
    FResult = Ok;
    FLoop->quit();
  }
}

void AuthenticateDialog::DoCancel()
{
  if (FOnCancel)
  {
    FOnCancel();
  }
}

void AuthenticateDialog::reject()
{
  if (FLoop != nullptr)
  {
    Finish(false);
  }
  else
  {
    DoCancel();
  }
}

void AuthenticateDialog::closeEvent(QCloseEvent * Event)
{
  Event->ignore();
  reject();
}

