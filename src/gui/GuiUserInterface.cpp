// Port of WinSCP source/windows/WinInterface.cpp (MessageDialog, countdown,
// "Never ask me again"), source/forms/MessageDlg.cpp (TMessageForm),
// source/windows/TerminalManager.cpp (Authenticate window, banner) and
// source/core/SecureShell.cpp (host key prompt texts and buttons)
#include "gui/GuiUserInterface.h"
#include "gui/Translator.h"

#include "core/Configuration.h"
#include "core/IniStorage.h"
#include "core/SessionData.h"
#include "gui/AuthenticateDialog.h"
#include "gui/HelpSystem.h"
#include "gui/Icons.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCryptographicHash>
#include <QCursor>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

//---------------------------------------------------------------------------
// Helpers

static QString Mnemonicless(QString S)
{
  return S.remove(QLatin1Char('&'));
}

// Accelerator character of a caption ("Yes to A&ll" -> 'l'), null if none
static QChar AccelChar(const QString & Caption)
{
  for (int Index = 0; Index + 1 < Caption.size(); Index++)
  {
    if (Caption.at(Index) == QLatin1Char('&'))
    {
      if (Caption.at(Index + 1) == QLatin1Char('&'))
      {
        Index++;
        continue;
      }
      return Caption.at(Index + 1).toLower();
    }
  }
  return QChar();
}

static bool IsPositiveAnswer(unsigned int Answer)
{
  return (Answer == qaYes) || (Answer == qaOK) || (Answer == qaYesToAll);
}

// CancelAnswer (windows/WinInterface.cpp)
static unsigned int CancelAnswerOf(unsigned int Answers)
{
  if (Answers & qaCancel) return qaCancel;
  if (Answers & qaNo) return qaNo;
  if (Answers & qaAbort) return qaAbort;
  if (Answers & qaOK) return qaOK;
  return qaCancel;
}

// AnswerNameAndCaption (forms/MessageDlg.cpp), in WinSCP's qaFirst..qaLast order
static const struct { unsigned int Answer; const char * Name; const char * Caption; } AnswerCaptions[] =
{
  { qaYes, "Yes", "&Yes" },
  { qaNo, "No", "&No" },
  { qaOK, "OK", "OK" },
  { qaCancel, "Cancel", "Cancel" },
  { qaYesToAll, "YesToAll", "Yes to A&ll" },
  { qaNoToAll, "NoToAll", "N&o to All" },
  { qaAbort, "Abort", "&Abort" },
  { qaRetry, "Retry", "&Retry" },
  { qaIgnore, "Ignore", "&Ignore" },
  { qaSkip, "Skip", "&Skip" },
  { qaAll, "All", "A&ll" },
  { qaHelp, "Help", "&Help" },
};

static QString ModifierName(Qt::KeyboardModifiers Modifiers)
{
  // ShortCutToText (Vcl.Menus.pas): GetKeyNameStr(VK_MENU / VK_CONTROL / VK_SHIFT)
  if (Modifiers == Qt::AltModifier) return QStringLiteral("Alt");
  if (Modifiers == Qt::ControlModifier) return QStringLiteral("Ctrl");
  if (Modifiers == Qt::ShiftModifier) return QStringLiteral("Shift");
  return QString();
}

static Qt::KeyboardModifiers ModifierOfKey(int Key)
{
  switch (Key)
  {
    case Qt::Key_Shift: return Qt::ShiftModifier;
    case Qt::Key_Control: return Qt::ControlModifier;
    case Qt::Key_Alt: return Qt::AltModifier;
    case Qt::Key_Meta: return Qt::MetaModifier;
    default: return Qt::NoModifier;
  }
}

// "**Main instruction**\n\nText" (MainInstructions markup)
static void SplitMainInstructions(const QString & Message, QString & Main, QString & Text)
{
  Main.clear();
  Text = Message;
  if (Message.startsWith(QStringLiteral("**")))
  {
    int End = Message.indexOf(QStringLiteral("**"), 2);
    if (End > 2)
    {
      Main = Message.mid(2, End - 2);
      Text = Message.mid(End + 2);
      while (Text.startsWith(QLatin1Char('\n')) || Text.startsWith(QLatin1Char('\r')))
      {
        Text.remove(0, 1);
      }
    }
  }
}

//---------------------------------------------------------------------------
// Help

static std::function<void(const QUrl &)> & OpenUrlHandler()
{
  static std::function<void(const QUrl &)> Handler;
  return Handler;
}

void SetOpenUrlHandler(std::function<void(const QUrl &)> Handler)
{
  OpenUrlHandler() = std::move(Handler);
}

void OpenBrowser(const QUrl & Url)
{
  if (OpenUrlHandler())
  {
    OpenUrlHandler()(Url);
  }
  else
  {
    QDesktopServices::openUrl(Url);
  }
}

void OpenHelp(const QString & HelpKeyword, QWidget * Parent, const QString & Message)
{
  if (!HelpKeyword.isEmpty())
  {
    // DOCUMENTATION_KEYWORD_URL2
    OpenBrowser(HelpSystem::KeywordUrl(HelpKeyword));
  }
  else
  {
    // MessageWithNoHelp: HELP_SEND_MESSAGE2, then SearchHelp (DOCUMENTATION_SEARCH_URL3)
    MessageParams Params;
    Params.AllowHelp = false;
    if (MessageDialog(Parent,
          QStringLiteral("**Do you want to send the message to WinSCP site?**\n\n"
            "There is no help page associated with the message. NixSCP can search WinSCP documentation site "
            "for the message text for you.\n\n"
            "Note: NixSCP will send the message as is over unsecure Internet connection. Please check that the "
            "message does not contain any data you want to protect, such as names of files, accounts or hosts."),
          qtConfirmation, qaOK | qaCancel, QString(), &Params) == qaOK)
    {
      QUrl Url(QStringLiteral("https://winscp.net/eng/docs/search.php"));
      Url.setQuery(QStringLiteral("q=%1").arg(QString::fromLatin1(QUrl::toPercentEncoding(Message))));
      OpenBrowser(Url);
    }
  }
}

//---------------------------------------------------------------------------
// MessageButtonAlias / MessageParams

MessageButtonAlias MessageButtonAlias::From(const QueryButtonAlias & Alias)
{
  MessageButtonAlias Result;
  Result.Button = Alias.Button;
  Result.Alias = Alias.Alias;
  Result.GroupWith = Alias.GroupWith;
  Result.GroupedModifiers = Alias.GroupedModifiers;
  return Result;
}

MessageParams MessageParams::From(const QueryUserParams & Params)
{
  MessageParams Result;
  Result.Params = Params.Params;
  for (const QueryButtonAlias & Alias : Params.Aliases)
  {
    Result.Aliases.append(MessageButtonAlias::From(Alias));
  }
  Result.Timeout = Params.Timeout;
  Result.TimeoutAnswer = Params.TimeoutAnswer;
  return Result;
}

//---------------------------------------------------------------------------
// MessageForm

MessageForm::MessageForm(QWidget * Parent, const QString & Message, const QString & MoreMessages, QueryType Type,
  unsigned int Answers, const QString & HelpKeyword, const MessageParams * Params) :
  QDialog(Parent),
  // central message path: WinSCP's message templates in the UI language
  FMessage(Translator::TrMessage(Message)),
  FMoreMessages(Translator::TrMessage(MoreMessages)),
  FHelpKeyword(HelpKeyword),
  FParams((Params != nullptr) ? *Params : MessageParams())
{
  setObjectName(QStringLiteral("MessageForm"));
  unsigned int ActualAnswers = Answers;
  // MessageDialog adds Help (qaHelp) unless AllowHelp is off. NixSCP: only
  // messages with a help page get it (no "search the WinSCP site for the
  // message" fallback of MessageWithNoHelp)
  if (FParams.AllowHelp && !FHelpKeyword.isEmpty())
  {
    Answers |= qaHelp;
    // F1 (FormHelp)
    HelpSystem::InstallFormHelp(this, FHelpKeyword);
  }
  FAnswers = Answers;
  FCancelAnswer = CancelAnswerOf(ActualAnswers);

  // In general, we should not have Yes and OK on the same query.
  // But we do for "hostkey" prompt, where they are aliased and OK is groupped with Yes.
  unsigned int DefaultAnswer = (Answers & qaYes) ? qaYes : ((Answers & qaOK) ? qaOK : qaRetry);

  QString ImageName;
  switch (Type)
  {
    case qtWarning: setWindowTitle(QStringLiteral("Warning")); ImageName = QStringLiteral("Warning"); break;
    case qtError: setWindowTitle(QStringLiteral("Error")); ImageName = QStringLiteral("Error"); break;
    case qtInformation: setWindowTitle(QStringLiteral("Information")); ImageName = QStringLiteral("Information"); break;
    case qtConfirmation:
    default: setWindowTitle(QStringLiteral("Confirm")); ImageName = QStringLiteral("Help Blue"); break;
  }
  if (!FParams.CustomCaption.isEmpty())
  {
    setWindowTitle(FParams.CustomCaption);
  }

  auto * Layout = new QVBoxLayout(this);
  auto * Contents = new QHBoxLayout();
  Contents->setSpacing(12);
  auto * Image = new QLabel(this);
  Image->setObjectName(QStringLiteral("Image"));
  QIcon Icon = Icons::DialogImage(ImageName);
  if (!Icon.isNull())
  {
    Image->setPixmap(Icon.pixmap(32, 32));
  }
  Image->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
  Contents->addWidget(Image, 0, Qt::AlignTop);
  auto * TextLayout = new QVBoxLayout();
  QString Main, Text;
  SplitMainInstructions(Message, Main, Text);
  if (!Main.isEmpty())
  {
    FMainLabel = new QLabel(Main, this);
    FMainLabel->setObjectName(QStringLiteral("MainInstruction"));
    FMainLabel->setTextFormat(Qt::PlainText);
    FMainLabel->setWordWrap(true);
    FMainLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont MainFont = FMainLabel->font();
    MainFont.setBold(true);
    MainFont.setPointSizeF(MainFont.pointSizeF() * 1.15);
    FMainLabel->setFont(MainFont);
    TextLayout->addWidget(FMainLabel);
  }
  FMessageLabel = new QLabel(Text, this);
  FMessageLabel->setObjectName(QStringLiteral("Message"));
  FMessageLabel->setTextFormat(Qt::PlainText);
  FMessageLabel->setWordWrap(true);
  FMessageLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  // mcMinDialogWidth; long messages (host key prompt) get a wider box instead of a tall narrow one
  FMessageLabel->setMinimumWidth((Text.size() > 300) ? std::max(310, FMessageLabel->fontMetrics().averageCharWidth() * 75) : 310);
  FMessageLabel->setMaximumWidth(std::max(560, FMessageLabel->fontMetrics().horizontalAdvance(QLatin1Char('x')) * 90));
  FMessageLabel->setVisible(!Text.isEmpty());
  TextLayout->addWidget(FMessageLabel);
  TextLayout->addStretch(1);
  Contents->addLayout(TextLayout, 1);
  Layout->addLayout(Contents);

  if (!MoreMessages.isEmpty())
  {
    auto * Memo = new QPlainTextEdit(MoreMessages, this);
    Memo->setObjectName(QStringLiteral("MessageMemo"));
    Memo->setReadOnly(true);
    Memo->setFixedHeight(std::max(86, Memo->fontMetrics().height() * 5));
    Layout->addWidget(Memo);
  }

  auto * ButtonRow = new QHBoxLayout();
  ButtonRow->setSpacing(6);
  QMap<unsigned int, ButtonInfo *> AnswerButtons;
  for (const auto & AC : AnswerCaptions)
  {
    unsigned int Answer = AC.Answer;
    if ((Answers & Answer) == 0)
    {
      continue;
    }
    QString Caption = QString::fromLatin1(AC.Caption);
    unsigned int GroupWith = 0;
    Qt::KeyboardModifiers GroupedModifiers = Qt::NoModifier;
    QString ActionAlias;
    bool HasSubmit = false;
    if (const MessageButtonAlias * Alias = FindAlias(Answer))
    {
      if (!Alias->Alias.isEmpty())
      {
        Caption = Alias->Alias;
      }
      GroupWith = Alias->GroupWith;
      GroupedModifiers = Alias->GroupedModifiers;
      ActionAlias = Alias->ActionAlias;
      HasSubmit = static_cast<bool>(Alias->OnSubmit);
    }

    // implemented for a one link only (as WinSCP)
    if (!ActionAlias.isEmpty() && (FLinkLabel == nullptr) && HasSubmit && (GroupWith == 0))
    {
      FLinkLabel = new QLabel(QStringLiteral("<a href=\"#\">%1</a>").arg(Mnemonicless(ActionAlias).toHtmlEscaped()), this);
      FLinkLabel->setObjectName(QString::fromLatin1(AC.Name));
      FLinkLabel->setAlignment(Qt::AlignRight);
      connect(FLinkLabel, &QLabel::linkActivated, this, [this, Answer]() { SubmitAnswer(Answer); });
      TextLayout->insertWidget(TextLayout->count() - 1, FLinkLabel);
      continue;
    }

    if ((GroupWith != 0) &&
        ((GroupWith == Answer) || (Answer == FParams.TimeoutAnswer) || (Answer == DefaultAnswer) ||
         (Answer == FCancelAnswer) || !AnswerButtons.contains(GroupWith)))
    {
      GroupWith = 0;
    }

    if (GroupWith != 0)
    {
      ButtonInfo * Group = AnswerButtons.value(GroupWith);
      if (Group->Menu == nullptr)
      {
        Group->Menu = new QMenu(this);
        Group->DropDown = new QToolButton(this);
        Group->DropDown->setObjectName(Group->Button->objectName() + QStringLiteral("DropDown"));
        Group->DropDown->setArrowType(Qt::DownArrow);
        Group->DropDown->setPopupMode(QToolButton::InstantPopup);
        Group->DropDown->setMenu(Group->Menu);
        Group->DropDown->setFocusPolicy(Qt::NoFocus);
        Group->DropDown->setFixedHeight(Group->Button->sizeHint().height());
        int Index = ButtonRow->indexOf(Group->Button);
        ButtonRow->insertWidget(Index + 1, Group->DropDown);
        QAction * Default = Group->Menu->addAction(Group->OwnCaption);
        Default->setData(Group->OwnAnswer);
        Default->setProperty("NixSCPModifiers", 0);
        Default->setProperty("NixSCPDefault", true);
        QFont DefaultFont = Default->font();
        DefaultFont.setBold(true);
        Default->setFont(DefaultFont);
        connect(Default, &QAction::triggered, this, [this, Default]() { MenuItemClicked(Default); });
      }
      QString ItemCaption = Caption;
      QString ModName = ModifierName(GroupedModifiers);
      if (!ModName.isEmpty())
      {
        ItemCaption += QLatin1Char('\t') + ModName;
      }
      QAction * Item = Group->Menu->addAction(ItemCaption);
      Item->setData(Answer);
      Item->setProperty("NixSCPModifiers", static_cast<int>(HasSubmit ? Qt::NoModifier : GroupedModifiers.toInt()));
      Item->setProperty("NixSCPDefault", false);
      connect(Item, &QAction::triggered, this, [this, Item]() { MenuItemClicked(Item); });
      continue;
    }

    auto * Info = new ButtonInfo();
    Info->OwnAnswer = Answer;
    Info->CurrentAnswer = Answer;
    Info->OwnCaption = Caption;
    Info->Button = new QPushButton(Caption, this);
    Info->Button->setObjectName(QString::fromLatin1(AC.Name));
    Info->Button->setMinimumWidth(80);
    if (Answer == FParams.TimeoutAnswer)
    {
      // room for the countdown
      Info->Button->setMinimumWidth(std::max(80, Info->Button->fontMetrics().horizontalAdvance(
        QStringLiteral("%1 (%2 s)").arg(Mnemonicless(Caption)).arg(99)) + 24));
    }
    connect(Info->Button, &QPushButton::clicked, this, [this, Info]() { ButtonClicked(Info); });
    FButtons.append(Info);
    AnswerButtons.insert(Answer, Info);
    ButtonRow->addWidget(Info->Button);
    if (Answer == DefaultAnswer)
    {
      Info->Button->setDefault(true);
    }
  }
  // The drop-down arrows go right after their buttons, no gap
  for (ButtonInfo * Info : FButtons)
  {
    if (Info->DropDown != nullptr)
    {
      Info->DropDown->setContentsMargins(0, 0, 0, 0);
    }
  }

  auto * BottomRow = new QHBoxLayout();
  bool HasNeverAskAgain = (FParams.Params & qpNeverAskAgainCheck) != 0;
  if (HasNeverAskAgain && !FButtons.isEmpty())
  {
    QString NeverAskAgainCaption = !FParams.NeverAskAgainTitle.isEmpty() ? FParams.NeverAskAgainTitle :
      // qaOK | qaIgnore is used, when custom "non-answer" button is required
      (((ActualAnswers == qaOK) || (ActualAnswers == (qaOK | qaIgnore))) ?
        QStringLiteral("Never show this message a&gain") : QStringLiteral("Never ask me a&gain"));
    FNeverAskAgainCheck = new QCheckBox(NeverAskAgainCaption, this);
    FNeverAskAgainCheck->setObjectName(QStringLiteral("NeverAskAgainCheck"));
    FNeverAskAgainCheck->setChecked(FParams.NeverAskAgainCheckedInitially);
    connect(FNeverAskAgainCheck, &QCheckBox::toggled, this, &MessageForm::NeverAskAgainCheckClick);
    BottomRow->addWidget(FNeverAskAgainCheck);
  }
  BottomRow->addStretch(1);
  BottomRow->addLayout(ButtonRow);
  Layout->addSpacing(6);
  Layout->addLayout(BottomRow);
  Layout->setSizeConstraint(QLayout::SetFixedSize);

  if (!FButtons.isEmpty())
  {
    FButtons.first()->Button->setFocus();
  }
  if (FNeverAskAgainCheck != nullptr && FNeverAskAgainCheck->isChecked())
  {
    NeverAskAgainCheckClick();
  }

  // Countdown (TMessageTimeoutButton)
  if ((FParams.Timeout > 0) && (FParams.TimeoutAnswer != 0))
  {
    FTimeoutInfo = AnswerButtons.value(FParams.TimeoutAnswer);
  }
  if (FTimeoutInfo != nullptr)
  {
    FOrigTimeout = static_cast<int>(FParams.Timeout);
    FTimeout = FOrigTimeout;
    FOrigCursorPos = QCursor::pos();
    FTimeoutTimer = new QTimer(this);
    FTimeoutTimer->setInterval(1000);
    connect(FTimeoutTimer, &QTimer::timeout, this, &MessageForm::TimeoutTick);
    FTimeoutTimer->start();
    UpdateTimeoutButton();
  }
}

MessageForm::~MessageForm()
{
  qApp->removeEventFilter(this);
  qDeleteAll(FButtons);
}

const MessageButtonAlias * MessageForm::FindAlias(unsigned int Answer) const
{
  for (const MessageButtonAlias & Alias : FParams.Aliases)
  {
    if (Alias.Button == Answer)
    {
      return &Alias;
    }
  }
  return nullptr;
}

MessageForm::ButtonInfo * MessageForm::InfoOf(QAbstractButton * Button) const
{
  for (ButtonInfo * Info : FButtons)
  {
    if (Info->Button == Button)
    {
      return Info;
    }
  }
  return nullptr;
}

QAbstractButton * MessageForm::AnswerButton(unsigned int Answer) const
{
  for (ButtonInfo * Info : FButtons)
  {
    if (Info->OwnAnswer == Answer)
    {
      return Info->Button;
    }
  }
  return nullptr;
}

unsigned int MessageForm::ButtonAnswer(QAbstractButton * Button) const
{
  ButtonInfo * Info = InfoOf(Button);
  return (Info != nullptr) ? Info->CurrentAnswer : 0;
}

QMenu * MessageForm::DropDownMenu(unsigned int Answer) const
{
  for (ButtonInfo * Info : FButtons)
  {
    if (Info->OwnAnswer == Answer)
    {
      return Info->Menu;
    }
  }
  return nullptr;
}

QPushButton * MessageForm::TimeoutButton() const
{
  return (FTimeoutInfo != nullptr) ? FTimeoutInfo->Button : nullptr;
}

void MessageForm::UpdateForModifiers(Qt::KeyboardModifiers Modifiers)
{
  // TMessageForm::UpdateForShiftState
  Modifiers &= (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier);
  FModifiers = Modifiers;
  for (ButtonInfo * Info : FButtons)
  {
    if (Info->Menu == nullptr)
    {
      continue;
    }
    QAction * Match = nullptr;
    QAction * Default = nullptr;
    for (QAction * Item : Info->Menu->actions())
    {
      bool IsDefault = Item->property("NixSCPDefault").toBool();
      if (IsDefault)
      {
        Default = Item;
      }
      Qt::KeyboardModifiers ItemModifiers = Qt::KeyboardModifiers::fromInt(Item->property("NixSCPModifiers").toInt());
      if (Item->isEnabled() &&
          (((Modifiers == Qt::NoModifier) && IsDefault) ||
           ((Modifiers != Qt::NoModifier) && (Modifiers == ItemModifiers))))
      {
        Match = Item;
        break;
      }
    }
    if (Match == nullptr)
    {
      Match = Default;
    }
    if (Match != nullptr)
    {
      QString Caption = Match->text();
      int Tab = Caption.indexOf(QLatin1Char('\t'));
      if (Tab >= 0)
      {
        Caption.truncate(Tab);
      }
      Info->CurrentAnswer = Match->data().toUInt();
      Info->Button->setText(Caption);
    }
  }
  if (FTimeoutInfo != nullptr)
  {
    UpdateTimeoutButton();
  }
}

void MessageForm::ButtonClicked(ButtonInfo * Info)
{
  SubmitAnswer(Info->CurrentAnswer);
}

void MessageForm::MenuItemClicked(QAction * Action)
{
  SubmitAnswer(Action->data().toUInt());
}

void MessageForm::SubmitAnswer(unsigned int Answer)
{
  if (Answer == qaHelp)
  {
    HelpButtonSubmit();
    return;
  }
  const MessageButtonAlias * Alias = FindAlias(Answer);
  if ((Alias != nullptr) && Alias->OnSubmit)
  {
    // TMessageForm::ButtonSubmit
    unsigned int Result = Alias->OnSubmit();
    if (Result != 0)
    {
      Finish(Result);
    }
    return;
  }
  Finish(Answer);
}

void MessageForm::HelpButtonSubmit()
{
  QString Text = FMessage;
  Text.remove(QStringLiteral("**"));
  OpenHelp(FHelpKeyword, this, Text);
}

void MessageForm::Finish(unsigned int Answer)
{
  FAnswer = Answer;
  if (FTimeoutTimer != nullptr)
  {
    FTimeoutTimer->stop();
  }
  done(QDialog::Accepted);
}

void MessageForm::reject()
{
  // mrCancel (X button, Escape) -> the valid "cancel" answer
  Finish(FCancelAnswer);
}

void MessageForm::NeverAskAgainCheckClick()
{
  bool Checked = FNeverAskAgainCheck->isChecked();
  unsigned int PositiveAnswer = 0;
  if (Checked)
  {
    if (FParams.NeverAskAgainAnswer > 0)
    {
      PositiveAnswer = FParams.NeverAskAgainAnswer;
    }
    else
    {
      for (ButtonInfo * Info : FButtons)
      {
        if (IsPositiveAnswer(Info->CurrentAnswer))
        {
          PositiveAnswer = Info->CurrentAnswer;
          break;
        }
      }
    }
  }
  for (ButtonInfo * Info : FButtons)
  {
    if ((Info->OwnAnswer != qaHelp) && (Info->CurrentAnswer != qaCancel))
    {
      Info->Button->setEnabled(!Checked || (Info->CurrentAnswer == PositiveAnswer));
    }
    if (Info->Menu != nullptr)
    {
      for (QAction * Item : Info->Menu->actions())
      {
        Item->setEnabled(Item->property("NixSCPDefault").toBool() || !Checked);
      }
      if (Info->DropDown != nullptr)
      {
        Info->DropDown->setEnabled(Info->Button->isEnabled());
      }
    }
  }
}

void MessageForm::UpdateTimeoutButton()
{
  if (FTimeoutInfo == nullptr)
  {
    return;
  }
  QString Caption = FTimeoutInfo->Button->text();
  int P = Caption.lastIndexOf(QStringLiteral(" ("));
  if ((P >= 0) && Caption.endsWith(QStringLiteral(" s)")))
  {
    Caption.truncate(P);
  }
  if ((FTimeoutTimer != nullptr) && FTimeoutTimer->isActive())
  {
    // TIMEOUT_BUTTON
    Caption = QStringLiteral("%1 (%2 s)").arg(Caption).arg(FTimeout / 1000);
  }
  FTimeoutInfo->Button->setText(Caption);
}

void MessageForm::TimeoutTick()
{
  if ((FTimeoutTimer == nullptr) || !FTimeoutTimer->isActive())
  {
    return;
  }
  // TMessageTimeout::MouseMove (polled): moving the mouse suspends the countdown
  QPoint CursorPos = QCursor::pos();
  int Delta = std::max(std::abs(FOrigCursorPos.x() - CursorPos.x()), std::abs(FOrigCursorPos.y() - CursorPos.y()));
  if (Delta > 8)
  {
    FOrigCursorPos = CursorPos;
    SuspendTimeout();
    return;
  }
  int Interval = FTimeoutTimer->interval();
  if (FTimeout <= Interval)
  {
    FTimeoutTimer->stop();
    UpdateTimeoutButton();
    // TMessageTimeoutButton::Timeouted
    if (FTimeoutInfo->Button->isEnabled())
    {
      SubmitAnswer(FTimeoutInfo->CurrentAnswer);
    }
  }
  else
  {
    FTimeout -= Interval;
    UpdateTimeoutButton();
  }
}

void MessageForm::SuspendTimeout()
{
  const int SuspendTime = 30 * 1000;
  FTimeout = std::max(FOrigTimeout, SuspendTime);
  UpdateTimeoutButton();
}

void MessageForm::CancelTimeout()
{
  if ((FTimeoutTimer != nullptr) && FTimeoutTimer->isActive())
  {
    FTimeoutTimer->stop();
    UpdateTimeoutButton();
  }
}

bool MessageForm::eventFilter(QObject * Watched, QEvent * Event)
{
  // TMessageTimeout::ApplicationMessage: a click or a key cancels the countdown
  if ((FTimeoutTimer != nullptr) && FTimeoutTimer->isActive() &&
      ((Event->type() == QEvent::MouseButtonPress) || (Event->type() == QEvent::KeyPress)))
  {
    QWidget * Widget = qobject_cast<QWidget *>(Watched);
    if ((Widget != nullptr) && (Widget->window() == this))
    {
      CancelTimeout();
    }
  }
  return QDialog::eventFilter(Watched, Event);
}

QString MessageForm::FormText() const
{
  // TMessageForm::GetFormText
  const QString DividerLine = QStringLiteral("---------------------------\n");
  QString ButtonCaptions;
  for (ButtonInfo * Info : FButtons)
  {
    ButtonCaptions += Mnemonicless(Info->Button->text()) + QStringLiteral("   ");
  }
  QString Message = FMessage;
  Message.remove(QStringLiteral("**"));
  QString Result = DividerLine + windowTitle() + QLatin1Char('\n') + DividerLine + Message.trimmed() + QLatin1Char('\n');
  if (!FMoreMessages.isEmpty())
  {
    Result += DividerLine + FMoreMessages.trimmed() + QLatin1Char('\n');
  }
  Result += DividerLine + ButtonCaptions.trimmed() + QLatin1Char('\n') + DividerLine;
  return Result;
}

void MessageForm::keyPressEvent(QKeyEvent * Event)
{
  Qt::KeyboardModifiers Modifiers = Event->modifiers() | ModifierOfKey(Event->key());
  if ((Event->modifiers() & Qt::ControlModifier) && (Event->key() == Qt::Key_C))
  {
    QApplication::clipboard()->setText(FormText());
    return;
  }
  if (ModifierOfKey(Event->key()) != Qt::NoModifier)
  {
    UpdateForModifiers(Modifiers);
    return;
  }
  if (!(Event->modifiers() & Qt::ControlModifier) && !Event->text().isEmpty() && Event->text().at(0).isLetterOrNumber())
  {
    QChar Key = Event->text().at(0).toLower();
    // Accelerators of the drop-down items (TMessageForm::KeyDown, IsAccel)
    for (ButtonInfo * Info : FButtons)
    {
      if (Info->Menu != nullptr)
      {
        for (QAction * Item : Info->Menu->actions())
        {
          if (Item->isEnabled() && (AccelChar(Item->text()) == Key))
          {
            MenuItemClicked(Item);
            return;
          }
        }
      }
    }
    // and of the buttons (Windows dialogs accept them without Alt too)
    for (ButtonInfo * Info : FButtons)
    {
      if (Info->Button->isEnabled() && (AccelChar(Info->Button->text()) == Key))
      {
        Info->Button->click();
        return;
      }
    }
    if ((FNeverAskAgainCheck != nullptr) && (AccelChar(FNeverAskAgainCheck->text()) == Key))
    {
      FNeverAskAgainCheck->toggle();
      return;
    }
  }
  QDialog::keyPressEvent(Event);
}

void MessageForm::keyReleaseEvent(QKeyEvent * Event)
{
  if (ModifierOfKey(Event->key()) != Qt::NoModifier)
  {
    UpdateForModifiers(Event->modifiers() & ~ModifierOfKey(Event->key()));
    return;
  }
  QDialog::keyReleaseEvent(Event);
}

unsigned int MessageForm::Answer() const
{
  unsigned int Result = (FAnswer != 0) ? FAnswer : FCancelAnswer;
  if ((FNeverAskAgainCheck != nullptr) && FNeverAskAgainCheck->isChecked())
  {
    bool PositiveAnswer = (FParams.NeverAskAgainAnswer > 0) ? (Result == FParams.NeverAskAgainAnswer) : IsPositiveAnswer(Result);
    if (PositiveAnswer)
    {
      Result = qaNeverAskAgain;
    }
  }
  return Result;
}

unsigned int MessageForm::Execute()
{
  QApplication::beep();
  if (FTimeoutTimer != nullptr)
  {
    qApp->installEventFilter(this);
  }
  exec();
  qApp->removeEventFilter(this);
  return Answer();
}

unsigned int MoreMessageDialog(QWidget * Parent, const QString & Message, const QString & MoreMessages, QueryType Type,
  unsigned int Answers, const QString & HelpKeyword, const MessageParams * Params)
{
  MessageForm Form(Parent, Message, MoreMessages, Type, Answers, HelpKeyword, Params);
  return Form.Execute();
}

unsigned int MessageDialog(QWidget * Parent, const QString & Message, QueryType Type, unsigned int Answers,
  const QString & HelpKeyword, const MessageParams * Params)
{
  return MoreMessageDialog(Parent, Message, QString(), Type, Answers, HelpKeyword, Params);
}

void ShowErrorMessage(QWidget * Parent, const QString & Message, const QString & MoreMessages)
{
  MoreMessageDialog(Parent, Mnemonicless(Message), MoreMessages, qtError, qaOK);
}

//---------------------------------------------------------------------------
// Host key

static QString StripFingerprintPrefix(QString Str, const QString & Prefix)
{
  if (Str.startsWith(Prefix, Qt::CaseInsensitive))
  {
    Str = Str.mid(Prefix.size());
  }
  return Str;
}

bool HostKeyMatchesText(const QString & Text, const QString & KeyType, const QString & FingerprintSHA256,
  const QString & FingerprintMD5)
{
  Q_UNUSED(KeyType);
  QString Str = Text.trimmed();
  if (Str.isEmpty())
  {
    return false;
  }
  QString Ours256 = StripFingerprintPrefix(FingerprintSHA256.trimmed(), QStringLiteral("SHA256:"));
  while (Ours256.endsWith(QLatin1Char('=')))
  {
    Ours256.chop(1);
  }
  QString OursMD5 = StripFingerprintPrefix(FingerprintMD5.trimmed(), QStringLiteral("MD5:")).toLower();

  // OpenSSH public key line: "<type> <base64 blob> [comment]" (ParseOpenSshPubLine)
  const QStringList Words = Str.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
  if ((Words.size() >= 2) && Words[1].startsWith(QStringLiteral("AAAA")))
  {
    QByteArray Blob = QByteArray::fromBase64(Words[1].toLatin1());
    QString Hash = QString::fromLatin1(QCryptographicHash::hash(Blob, QCryptographicHash::Sha256).toBase64(QByteArray::OmitTrailingEquals));
    return !Blob.isEmpty() && !Ours256.isEmpty() && (Hash == Ours256);
  }

  // VerifyFingerprint: "<type> <bits> <fingerprint>" or the fingerprint alone
  QString Fingerprint = Words.last();
  if (Fingerprint.startsWith(QStringLiteral("SHA256:"), Qt::CaseInsensitive) || !Fingerprint.contains(QLatin1Char(':')))
  {
    Fingerprint = StripFingerprintPrefix(Fingerprint, QStringLiteral("SHA256:"));
    while (Fingerprint.endsWith(QLatin1Char('=')))
    {
      Fingerprint.chop(1);
    }
    return !Ours256.isEmpty() && (Fingerprint == Ours256);
  }
  Fingerprint = StripFingerprintPrefix(Fingerprint, QStringLiteral("MD5:")).toLower();
  return !OursMD5.isEmpty() && (Fingerprint == OursMD5);
}

// "<type> <bits> <hash>" fingerprint format of TSecureShell::VerifyHostKey
static QString SignKeyType(const QString & KeyType)
{
  QString Bits;
  if (KeyType == QStringLiteral("ssh-ed25519")) Bits = QStringLiteral("255");
  else if (KeyType == QStringLiteral("ssh-ed448")) Bits = QStringLiteral("448");
  else if (KeyType.endsWith(QStringLiteral("nistp256"))) Bits = QStringLiteral("256");
  else if (KeyType.endsWith(QStringLiteral("nistp384"))) Bits = QStringLiteral("384");
  else if (KeyType.endsWith(QStringLiteral("nistp521"))) Bits = QStringLiteral("521");
  return Bits.isEmpty() ? KeyType : (KeyType + QLatin1Char(' ') + Bits);
}

//---------------------------------------------------------------------------
// GuiUserInterface

GuiUserInterface::GuiUserInterface(QWidget * ParentWidget) :
  QObject(ParentWidget),
  FParentWidget(ParentWidget)
{
}

GuiUserInterface::~GuiUserInterface()
{
  if (FAuthenticate != nullptr)
  {
    delete FAuthenticate.data();
  }
}

template<typename F>
auto GuiUserInterface::InGuiThread(F Func) -> decltype(Func())
{
  using R = decltype(Func());
  if (QThread::currentThread() == thread())
  {
    return Func();
  }
  R Result{};
  QMetaObject::invokeMethod(this, [&]() { Result = Func(); }, Qt::BlockingQueuedConnection);
  return Result;
}

AuthenticateDialog * GuiUserInterface::Authenticate() const
{
  return FAuthenticate;
}

QWidget * GuiUserInterface::DialogParent() const
{
  if ((FAuthenticate != nullptr) && FAuthenticate->isVisible())
  {
    return FAuthenticate;
  }
  return FParentWidget;
}

QString GuiUserInterface::SessionKey(const SessionData & Data)
{
  // TSessionData::GetSessionKey
  QString Result = QStringLiteral("%1@%2").arg(Data.UserName, Data.HostName);
  if (Data.PortNumber != Data.DefaultPort())
  {
    Result += QStringLiteral(":%1").arg(Data.PortNumber);
  }
  return Result;
}

QString GuiUserInterface::BannerHash(const QString & Banner)
{
  // TConfiguration::BannerHash: MD5 of the UTF-16 (wchar_t) string, BytesToHex (upper case)
  QByteArray Utf16(reinterpret_cast<const char *>(Banner.utf16()), Banner.size() * 2);
  return QString::fromLatin1(QCryptographicHash::hash(Utf16, QCryptographicHash::Md5).toHex().toUpper());
}

void GuiUserInterface::BeginConnecting(const SessionData & Data, std::function<void()> OnCancel)
{
  EndConnecting();
  FConnectData = std::make_unique<SessionData>(Data);
  FAuthenticatingLogged = false;
  FSessionRememberPassword = false;
  bool Secure =
    (Data.FSProtocol == fsSCPonly) || (Data.FSProtocol == fsSFTP) || (Data.FSProtocol == fsSFTPonly) ||
    (Data.Ftps != ftpsNone);
  FAuthenticate = new AuthenticateDialog(FParentWidget);
  FAuthenticate->Init(Data.SessionName(), Secure);
  FAuthenticate->SetOnCancel(std::move(OnCancel));
  FAuthenticate->Log(QStringLiteral("Searching for host..."));
  FAuthenticate->Log(QStringLiteral("Connecting to host..."));
  FAuthenticate->ShowAsModal();
}

void GuiUserInterface::EndConnecting()
{
  if (FAuthenticate != nullptr)
  {
    AuthenticateDialog * Dialog = FAuthenticate;
    FAuthenticate = nullptr;
    Dialog->SetOnCancel({});
    Dialog->Finish(false);
    Dialog->hide();
    Dialog->deleteLater();
  }
  FConnectData.reset();
}

void GuiUserInterface::Information(const QString & Message)
{
  if (QThread::currentThread() == thread())
  {
    if (FAuthenticate != nullptr)
    {
      FAuthenticate->Log(Message);
    }
  }
  else
  {
    QMetaObject::invokeMethod(this, [this, Message]() { Information(Message); }, Qt::QueuedConnection);
  }
}

bool GuiUserInterface::IsConnectingSession(const QString & Name) const
{
  return (FConnectData != nullptr) && (FConnectData->SessionName() == Name);
}

void GuiUserInterface::LogAuthenticating()
{
  if ((FAuthenticate != nullptr) && !FAuthenticatingLogged)
  {
    FAuthenticatingLogged = true;
    // STATUS_AUTHENTICATE
    FAuthenticate->Log(QStringLiteral("Authenticating..."));
    if ((FConnectData != nullptr) && !FConnectData->UserName.isEmpty())
    {
      FAuthenticate->Log(QStringLiteral("Using username \"%1\".").arg(FConnectData->UserName));
    }
  }
}

UserInterface::HostKeyAnswer GuiUserInterface::VerifyHostKey(const QString & Host, int Port, const QString & KeyType,
  const QString & FingerprintSHA256, const QString & FingerprintMD5, HostKeyState State)
{
  return InGuiThread([&]() -> HostKeyAnswer
  {
    if (Configuration::Instance().DisableAcceptingHostKeys())
    {
      // TSecureShell::VerifyHostKey: Configuration->DisableAcceptingHostKeys
      return HostKeyCancel;
    }
    bool Unknown = (State == HostKeyUnknown);
    QString SHA256 = StripFingerprintPrefix(FingerprintSHA256, QStringLiteral("SHA256:"));
    QString MD5 = StripFingerprintPrefix(FingerprintMD5, QStringLiteral("MD5:"));
    QString FullSHA256 = SignKeyType(KeyType) + QLatin1Char(' ') + SHA256;
    QString FullMD5 = SignKeyType(KeyType) + QLatin1Char(' ') + MD5;

    const QString AcceptButton = QStringLiteral("&Accept");       // HOSTKEY_ACCEPT_BUTTON
    const QString OnceButton = QStringLiteral("Connect &Once");   // HOSTKEY_ONCE_BUTTON
    const QString CancelButton = QStringLiteral("Cancel");
    const QString UpdateButton = QStringLiteral("&Update");       // UPDATE_KEY_BUTTON

    MessageParams Params;
    MessageButtonAlias CopyAlias;
    CopyAlias.Button = qaRetry;
    CopyAlias.Alias = QStringLiteral("&Copy Key");                       // COPY_KEY_BUTTON
    CopyAlias.ActionAlias = QStringLiteral("&Copy key fingerprints to clipboard"); // COPY_KEY_ACTION
    CopyAlias.OnSubmit = [FullSHA256, FullMD5]() -> unsigned int
    {
      QApplication::clipboard()->setText(FullSHA256 + QLatin1Char('\n') + FullMD5);
      return 0;
    };
    Params.Aliases.append(CopyAlias);

    MessageButtonAlias PasteAlias;
    PasteAlias.Button = qaIgnore;
    PasteAlias.Alias = QStringLiteral("&Paste key");                     // PASTE_KEY_BUTTON
    PasteAlias.GroupWith = qaYes;
    PasteAlias.OnSubmit = [this, KeyType, FingerprintSHA256, FingerprintMD5]() -> unsigned int
    {
      if (HostKeyMatchesText(QApplication::clipboard()->text(), KeyType, FingerprintSHA256, FingerprintMD5))
      {
        return qaYes;
      }
      MessageDialog(DialogParent(), QStringLiteral("Contents of the clipboard does not match the host key nor its fingerprint."),
        qtError, qaOK);
      return 0;
    };
    Params.Aliases.append(PasteAlias);

    MessageButtonAlias OnceAlias;
    OnceAlias.Button = qaOK;
    OnceAlias.Alias = OnceButton;
    OnceAlias.GroupWith = qaYes;
    Params.Aliases.append(OnceAlias);

    unsigned int Answers = qaYes | qaOK | qaCancel | qaRetry | qaIgnore;
    MessageButtonAlias YesAlias;
    YesAlias.Button = qaYes;
    YesAlias.Alias = Unknown ? AcceptButton : UpdateButton;
    Params.Aliases.append(YesAlias);

    const QString NewLine = QStringLiteral("\n");
    const QString Para = QStringLiteral("\n\n");
    const QString Nbsp = QString(QChar(0xA0));
    const QString Indent = Nbsp + Nbsp + Nbsp + Nbsp;
    QString ServerPara = QStringLiteral("%1 (port %2)").arg(Host).arg(Port) + Para;
    QString FingerprintPara =
      Indent + QStringLiteral("The %1 key fingerprint is:").arg(KeyType) + NewLine +
      Indent + QString(FullSHA256).replace(QLatin1Char(' '), Nbsp) + Para;
    QString Message;
    if (Unknown)
    {
      Message =
        QStringLiteral("**Continue connecting to an unknown server and add its host key to a cache?**") + Para +
        QStringLiteral("The host key is not cached for this server:") + NewLine +
        ServerPara +
        QStringLiteral("You have no guarantee that the server is the computer you think it is.") + Para;
    }
    else
    {
      Message =
        QStringLiteral("**Warning – Potential security breach!**") + Para +
        QStringLiteral("The host key does not match the one NixSCP has cached for this server:") + NewLine +
        ServerPara +
        QStringLiteral("This means that either the server administrator has changed the host key, "
          "or you have actually connected to another computer pretending to be the server.") + Para;
    }
    Message += FingerprintPara;
    if (Unknown)
    {
      Message +=
        QStringLiteral("If you trust this host, select %1 to add the key to NixSCP's cache and carry on connecting.").arg(Mnemonicless(AcceptButton)) + NewLine +
        QStringLiteral("If you want to carry on connecting just once, without adding the key to the cache, select %1.").arg(Mnemonicless(OnceButton)) + NewLine +
        QStringLiteral("If you do not trust this host, select %1 to abandon the connection.").arg(CancelButton);
    }
    else
    {
      Message +=
        QStringLiteral("If you were expecting this change, trust the new key and want to continue connecting to the server, "
          "select %1 to update cache.").arg(Mnemonicless(UpdateButton)) + NewLine +
        QStringLiteral("If you want to carry on connecting but without updating the cache, select %1.").arg(Mnemonicless(OnceButton)) + NewLine +
        QStringLiteral("If you want to abandon the connection completely, select %1 to cancel. "
          "Selecting %1 is the ONLY guaranteed safe choice.").arg(CancelButton);
    }

    unsigned int R = MoreMessageDialog(DialogParent(), Message, QString(), qtWarning, Answers,
      Unknown ? QStringLiteral("message_host_key") : QStringLiteral("message_security_breach"), &Params);
    switch (R)
    {
      case qaYes:
        return HostKeyAccept;
      case qaOK:
        return HostKeyOnce;
      default:
        return HostKeyCancel;
    }
  });
}

bool GuiUserInterface::PromptUser(PromptKind Kind, const QString & Name, const QString & Instructions, QList<Prompt> & Prompts)
{
  return InGuiThread([&]() -> bool
  {
    if (Kind == pkFileName)
    {
      // Not an authentication prompt: a plain input box titled Name
      QDialog Dialog(DialogParent());
      Dialog.setWindowTitle(Name);
      auto * Layout = new QVBoxLayout(&Dialog);
      if (!Instructions.isEmpty())
      {
        auto * Label = new QLabel(Instructions.trimmed());
        Label->setWordWrap(true);
        Layout->addWidget(Label);
      }
      auto * Form = new QFormLayout();
      QList<QLineEdit *> Edits;
      for (const Prompt & P : Prompts)
      {
        auto * Edit = new QLineEdit(P.Result);
        Edit->setEchoMode(P.Echo ? QLineEdit::Normal : QLineEdit::Password);
        Edit->setMinimumWidth(280);
        auto * Label = new QLabel(P.Text.trimmed());
        Label->setBuddy(Edit);
        Form->addRow(Label, Edit);
        Edits.append(Edit);
      }
      Layout->addLayout(Form);
      auto * Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
      connect(Buttons, &QDialogButtonBox::accepted, &Dialog, &QDialog::accept);
      connect(Buttons, &QDialogButtonBox::rejected, &Dialog, &QDialog::reject);
      Layout->addWidget(Buttons);
      if (!Edits.isEmpty())
      {
        Edits.first()->setFocus();
      }
      if (Dialog.exec() != QDialog::Accepted)
      {
        return false;
      }
      for (int I = 0; I < Prompts.size(); I++)
      {
        Prompts[I].Result = Edits[I]->text();
      }
      return true;
    }

    bool PasswordPrompt = (Kind == pkPassword) || ((Kind == pkKeybInteractive) && (Prompts.size() == 1) && !Prompts[0].Echo);
    bool PasswordOrPassphrasePrompt = PasswordPrompt || (Kind == pkPassphrase);

    // The session being opened prompts in its Authenticate window; others
    // (background connections, a second prompt while one is shown) get their own
    bool OwnSession = IsConnectingSession(Name) || ((Kind == pkKeybInteractive) && (FConnectData != nullptr));
    bool UseConnecting = (FAuthenticate != nullptr) && !FAuthenticate->Busy() && OwnSession;
    std::unique_ptr<AuthenticateDialog> Own;
    AuthenticateDialog * Dialog;
    QString SessionName;
    if (UseConnecting)
    {
      Dialog = FAuthenticate;
      SessionName = FConnectData->SessionName();
      LogAuthenticating();
    }
    else
    {
      Own = std::make_unique<AuthenticateDialog>(DialogParent());
      SessionName = Name;
      Own->Init(SessionName, true);
      Dialog = Own.get();
    }

    QString Title;
    switch (Kind)
    {
      case pkPassword: Title = QStringLiteral("Password"); break;           // PASSWORD_TITLE
      case pkPassphrase: Title = QStringLiteral("Key passphrase"); break;   // PASSPHRASE_TITLE
      case pkUserName: Title = QStringLiteral("Username"); break;           // USERNAME_TITLE
      default:
        // SERVER_PROMPT_TITLE / SERVER_PROMPT_TITLE2 (name given by the server)
        Title = (UseConnecting && (Name != SessionName)) ?
          QStringLiteral("Server prompt: %1").arg(Name) : QStringLiteral("Server prompt");
        break;
    }

    AuthenticateDialog::PromptOptions Options;
    // Change stored password to this one: the stored password of the site was tried and failed
    std::shared_ptr<SessionData> StoredSite;
    if (PasswordPrompt && UseConnecting && !FConnectData->Name.isEmpty() && !FConnectData->Password.isEmpty())
    {
      StoredSite = Configuration::Instance().StoredSessions().FindByName(FConnectData->Name);
      Options.ShowSavePassword = (StoredSite != nullptr) && !StoredSite->Password.isEmpty() &&
        !Configuration::Instance().DisablePasswordStoring();
    }
    // Remember password for this session (pupRemember): only when not remembered anyway,
    // and only for the session being opened (BeginConnecting), whose opener reads
    // the answer with TakeSessionRememberPassword(); elsewhere the box would have no effect
    const Configuration & Config = Configuration::Instance();
    Options.ShowSessionRememberPassword = UseConnecting &&
      (Prompts.size() == 1) && PasswordOrPassphrasePrompt && !(Config.SessionRememberPassword || Config.PuttyPassword);

    if (!Dialog->PromptUser(Kind, Title, Instructions, Prompts, Options))
    {
      return false;
    }
    if (Options.SessionRememberPassword)
    {
      FSessionRememberPassword = true;
    }
    if (Options.SavePassword && (StoredSite != nullptr) && !Prompts.isEmpty())
    {
      StoredSite->Password = Prompts[0].Result;
      FConnectData->Password = Prompts[0].Result;
      // modified only, explicit
      Configuration::Instance().Save();
    }
    if (PasswordPrompt)
    {
      FLastPassword = Prompts[0].Result;
    }
    return true;
  });
}

void GuiUserInterface::DisplayBanner(const QString & SessionName, const QString & Banner)
{
  if (Banner.trimmed().isEmpty())
  {
    return;
  }
  InGuiThread([&]() -> bool
  {
    // TTerminal::DisplayBanner
    Configuration & Config = Configuration::Instance();
    bool OwnSession = IsConnectingSession(SessionName);
    QString Key = OwnSession ? SessionKey(*FConnectData) : QString();
    QString StoredHash;
    unsigned int Params = 0;
    if (!Key.isEmpty())
    {
      // TConfiguration::GetBannerData
      IniStorage Storage(&Config.Ini());
      if (Storage.OpenSubKeyPath(QStringLiteral("Configuration\\Banners"), false))
      {
        QString S = Storage.ReadString(Key, QString());
        int Comma = S.indexOf(QLatin1Char(','));
        StoredHash = (Comma >= 0) ? S.left(Comma) : S;
        // "$" + hex (StrToIntDef)
        Params = (Comma >= 0) ? S.mid(Comma + 1).section(QLatin1Char(','), 0, 0).toUInt(nullptr, 16) : 0;
      }
    }
    QString Hash = BannerHash(Banner);
    bool ForceBanners = Config.ForceBanners();
    if (!ForceBanners && !Key.isEmpty() && (StoredHash == Hash))
    {
      return true;
    }

    bool NeverShowAgain = false;
    int Options = (ForceBanners || Key.isEmpty()) ? boDisableNeverShowAgain : 0;
    unsigned int OrigParams = Params;
    bool UseConnecting = OwnSession && (FAuthenticate != nullptr) && !FAuthenticate->Busy();
    if (UseConnecting)
    {
      LogAuthenticating();
      FAuthenticate->Banner(Banner, NeverShowAgain, Options, Params);
    }
    else
    {
      AuthenticateDialog Dialog(DialogParent());
      Dialog.Init(SessionName, true);
      Dialog.Banner(Banner, NeverShowAgain, Options, Params);
    }

    if (!Key.isEmpty() && !Config.IsNulStorage())
    {
      bool Changed = false;
      IniStorage Storage(&Config.Ini());
      if (!ForceBanners && NeverShowAgain)
      {
        // NeverShowBanner
        if (Storage.OpenSubKeyPath(QStringLiteral("Configuration\\Banners"), true))
        {
          Storage.WriteString(Key, Hash + QLatin1Char(',') + QString::number(Params));
          Changed = true;
        }
      }
      else if (Params != OrigParams)
      {
        // SetBannerParams
        if (Storage.OpenSubKeyPath(QStringLiteral("Configuration\\Banners"), true))
        {
          Storage.WriteString(Key, StoredHash + QLatin1Char(',') + QString::number(Params));
          Changed = true;
        }
      }
      if (Changed)
      {
        Config.Save();
      }
    }
    return true;
  });
}

unsigned int GuiUserInterface::QueryUser(const QString & Query, const QString & MoreMessages, unsigned int Answers)
{
  return QueryUserEx(Query, MoreMessages, Answers, QueryUserParams());
}

unsigned int GuiUserInterface::QueryUserEx(const QString & Query, const QString & MoreMessages, unsigned int Answers,
  const QueryUserParams & Params)
{
  return InGuiThread([&]() -> unsigned int
  {
    MessageParams MParams = MessageParams::From(Params);
    return MoreMessageDialog(DialogParent(), Query, MoreMessages, Params.Type, Answers, Params.HelpKeyword, &MParams);
  });
}

QString GuiUserInterface::TakeLastPassword()
{
  QString Result = FLastPassword;
  FLastPassword.clear();
  return Result;
}

bool GuiUserInterface::TakeSessionRememberPassword()
{
  bool Result = FSessionRememberPassword;
  FSessionRememberPassword = false;
  return Result;
}
