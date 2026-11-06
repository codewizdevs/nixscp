#include "mobile/MobileUserInterface.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QThread>
#include <QVariantMap>

namespace
{
// A connection's thread, for good (the system's thread ids come back)
quint64 ThisThread()
{
  static std::atomic<quint64> Last{0};
  thread_local const quint64 Tag = ++Last;
  return Tag;
}

// dismissed: the safest answer offered
unsigned int SafeAnswer(unsigned int Answers)
{
  for (unsigned int Safe : {static_cast<unsigned int>(qaCancel), static_cast<unsigned int>(qaAbort),
         static_cast<unsigned int>(qaNo), static_cast<unsigned int>(qaSkip), static_cast<unsigned int>(qaOK)})
  {
    if (Answers & Safe)
    {
      return Safe;
    }
  }
  return Answers & (~Answers + 1);   // the lowest offered
}
}

MobileUserInterface::MobileUserInterface(QObject * Parent) : QObject(Parent)
{
}

void MobileUserInterface::Ask(const std::shared_ptr<Question> & Item)
{
  {
    QMutexLocker Lock(&FMutex);
    FWaiting.append(Item);
  }
  QMetaObject::invokeMethod(this, [this]() { ShowNext(); }, Qt::QueuedConnection);
  if (QThread::currentThread() == thread())
  {
    // asked on the GUI thread: keep the interface alive until QML answers
    QEventLoop Loop;
    while (true)
    {
      {
        QMutexLocker Lock(&FMutex);
        if (Item->Answered)
        {
          break;
        }
      }
      Loop.processEvents(QEventLoop::AllEvents, 50);
    }
    return;
  }
  QMutexLocker Lock(&FMutex);
  while (!Item->Answered)
  {
    FAnswered.wait(&FMutex);
  }
}

// GUI thread: the next question, once the one shown is answered
void MobileUserInterface::ShowNext()
{
  std::shared_ptr<Question> Next;
  {
    QMutexLocker Lock(&FMutex);
    if ((FShown != nullptr) || FWaiting.isEmpty())
    {
      return;
    }
    Next = FWaiting.takeFirst();
    FShown = Next;
  }
  Next->Show();
}

void MobileUserInterface::Finish(const std::function<void(Question &)> & Fill)
{
  {
    QMutexLocker Lock(&FMutex);
    if (FShown == nullptr)
    {
      return;
    }
    Fill(*FShown);
    FShown->Answered = true;
    FShown.reset();
    FAnswered.wakeAll();
  }
  QMetaObject::invokeMethod(this, [this]() { ShowNext(); }, Qt::QueuedConnection);
}

void MobileUserInterface::CancelQuestions(const void * Owner)
{
  bool ShownCancelled = false;
  {
    QMutexLocker Lock(&FMutex);
    for (qsizetype I = FWaiting.size() - 1; I >= 0; I--)
    {
      if (FWaiting.at(I)->Owner == Owner)
      {
        FWaiting.at(I)->Cancelled = true;
        FWaiting.at(I)->Answered = true;
        FWaiting.removeAt(I);
      }
    }
    if ((FShown != nullptr) && (FShown->Owner == Owner))
    {
      FShown->Cancelled = true;
      FShown->Answered = true;
      FShown.reset();
      ShownCancelled = true;
    }
    FAnswered.wakeAll();
  }
  if (ShownCancelled)
  {
    emit questionCancelled();
    QMetaObject::invokeMethod(this, [this]() { ShowNext(); }, Qt::QueuedConnection);
  }
}

UserInterface::HostKeyAnswer MobileUserInterface::AskHostKey(const void * Owner, const QString & Host, int Port,
  const QString & KeyType, const QString & Fingerprint, HostKeyState State)
{
  auto Item = std::make_shared<Question>();
  Item->Owner = Owner;
  Item->Show = [=, this]() { emit hostKeyRequested(Host, Port, KeyType, Fingerprint, (State == HostKeyChanged) ? 1 : 0); };
  Ask(Item);
  QMutexLocker Lock(&FMutex);
  if (Item->Cancelled)
  {
    return HostKeyCancel;
  }
  switch (Item->Answer)
  {
    case 0: return HostKeyAccept;
    case 1: return HostKeyOnce;
    default: return HostKeyCancel;
  }
}

bool MobileUserInterface::AskPrompt(const void * Owner, PromptKind Kind, const QString & Name, const QString & Instructions,
  QList<Prompt> & Prompts)
{
  QVariantList List;
  for (const Prompt & P : Prompts)
  {
    List.append(QVariantMap{{QStringLiteral("text"), P.Text}, {QStringLiteral("echo"), P.Echo}});
  }
  auto Item = std::make_shared<Question>();
  Item->Owner = Owner;
  Item->Show = [=, this]() { emit promptRequested(static_cast<int>(Kind), Name, Instructions, List); };
  Ask(Item);
  QMutexLocker Lock(&FMutex);
  if (Item->Cancelled || !Item->PromptOk)
  {
    return false;
  }
  for (qsizetype I = 0; I < Prompts.size(); I++)
  {
    Prompts[I].Result = Item->Values.value(I);
  }
  return true;
}

unsigned int MobileUserInterface::AskQuery(const void * Owner, const QString & Query, const QString & MoreMessages, unsigned int Answers)
{
  QVariantList Buttons = QueryButtons(Answers);
  auto Item = std::make_shared<Question>();
  Item->Owner = Owner;
  Item->Show = [=, this]() { emit queryRequested(Query, MoreMessages, Buttons); };
  Ask(Item);
  QMutexLocker Lock(&FMutex);
  if (!Item->Cancelled && (Item->Answer > 0) && (static_cast<unsigned int>(Item->Answer) & Answers))
  {
    return static_cast<unsigned int>(Item->Answer);
  }
  return SafeAnswer(Answers);
}

UserInterface::HostKeyAnswer MobileUserInterface::VerifyHostKey(const QString & Host, int Port, const QString & KeyType,
  const QString & FingerprintSHA256, const QString & FingerprintMD5, HostKeyState State)
{
  Q_UNUSED(FingerprintMD5);
  return AskHostKey(this, Host, Port, KeyType, FingerprintSHA256, State);
}

bool MobileUserInterface::PromptUser(PromptKind Kind, const QString & Name, const QString & Instructions, QList<Prompt> & Prompts)
{
  return AskPrompt(this, Kind, Name, Instructions, Prompts);
}

unsigned int MobileUserInterface::QueryUser(const QString & Query, const QString & MoreMessages, unsigned int Answers)
{
  return AskQuery(this, Query, MoreMessages, Answers);
}

//---------------------------------------------------------------------------
void MobileUserInterface::Asker::Cancel()
{
  FCancelled = true;
  FOwner->CancelQuestions(this);
}

UserInterface::HostKeyAnswer MobileUserInterface::Asker::VerifyHostKey(const QString & Host, int Port, const QString & KeyType,
  const QString & FingerprintSHA256, const QString & FingerprintMD5, HostKeyState State)
{
  Q_UNUSED(FingerprintMD5);
  return FCancelled ? HostKeyCancel : FOwner->AskHostKey(this, Host, Port, KeyType, FingerprintSHA256, State);
}

bool MobileUserInterface::Asker::PromptUser(PromptKind Kind, const QString & Name, const QString & Instructions, QList<Prompt> & Prompts)
{
  if (FCancelled)
  {
    return false;
  }
  bool Passphrase = (Kind == pkPassphrase) && (Prompts.size() == 1);
  if (Passphrase)
  {
    QMutexLocker Lock(&FMutex);
    if (!FPassphrase.isEmpty() && (FPassphraseThread != ThisThread()))
    {
      FPassphraseThread = ThisThread();
      Prompts[0].Result = FPassphrase;
      return true;
    }
    FPassphrase.clear();
  }
  bool Ok = FOwner->AskPrompt(this, Kind, Name, Instructions, Prompts);
  if (Ok && Passphrase)
  {
    QMutexLocker Lock(&FMutex);
    FPassphrase = Prompts[0].Result;
    FPassphraseThread = ThisThread();
  }
  return Ok;
}

void MobileUserInterface::Asker::DisplayBanner(const QString & SessionName, const QString & Banner)
{
  if (!FCancelled)
  {
    FOwner->DisplayBanner(SessionName, Banner);
  }
}

unsigned int MobileUserInterface::Asker::QueryUser(const QString & Query, const QString & MoreMessages, unsigned int Answers)
{
  return FCancelled ? SafeAnswer(Answers) : FOwner->AskQuery(this, Query, MoreMessages, Answers);
}

//---------------------------------------------------------------------------
void MobileUserInterface::DisplayBanner(const QString & SessionName, const QString & Banner)
{
  QMetaObject::invokeMethod(this, [=, this]() { emit bannerShown(SessionName, Banner); }, Qt::QueuedConnection);
}

QVariantList MobileUserInterface::QueryButtons(unsigned int Answers)
{
  static const QList<QPair<unsigned int, const char *>> Order = {
    {qaYes, "Yes"}, {qaOK, "OK"}, {qaRetry, "Retry"}, {qaYesToAll, "Yes to All"}, {qaAll, "All"},
    {qaSkip, "Skip"}, {qaIgnore, "Ignore"}, {qaNo, "No"}, {qaNoToAll, "No to All"},
    {qaAbort, "Abort"}, {qaCancel, "Cancel"}};
  QVariantList Result;
  for (const auto & [Answer, Caption] : Order)
  {
    if (Answers & Answer)
    {
      Result.append(QVariantMap{{QStringLiteral("answer"), Answer}, {QStringLiteral("caption"), QString::fromLatin1(Caption)}});
    }
  }
  return Result;
}

void MobileUserInterface::answerHostKey(int Answer)
{
  Finish([Answer](Question & Item) { Item.Answer = Answer; });
}

void MobileUserInterface::answerPrompt(bool Ok, const QStringList & Values)
{
  Finish([Ok, &Values](Question & Item)
  {
    Item.PromptOk = Ok;
    Item.Values = Values;
  });
}

void MobileUserInterface::answerQuery(int Answer)
{
  Finish([Answer](Question & Item) { Item.Answer = Answer; });
}
