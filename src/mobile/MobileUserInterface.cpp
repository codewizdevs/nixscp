#include "mobile/MobileUserInterface.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QThread>
#include <QVariantMap>

MobileUserInterface::MobileUserInterface(QObject * Parent) : QObject(Parent)
{
}

void MobileUserInterface::Ask(const std::function<void()> & Emit)
{
  {
    QMutexLocker Lock(&FMutex);
    FHasAnswer = false;
  }
  if (QThread::currentThread() == thread())
  {
    // asked on the GUI thread: keep the interface alive until QML answers
    Emit();
    QEventLoop Loop;
    while (true)
    {
      {
        QMutexLocker Lock(&FMutex);
        if (FHasAnswer)
        {
          break;
        }
      }
      Loop.processEvents(QEventLoop::AllEvents, 50);
    }
    return;
  }
  QMetaObject::invokeMethod(this, [Emit]() { Emit(); }, Qt::QueuedConnection);
  QMutexLocker Lock(&FMutex);
  while (!FHasAnswer)
  {
    FAnswered.wait(&FMutex);
  }
}

UserInterface::HostKeyAnswer MobileUserInterface::VerifyHostKey(const QString & Host, int Port, const QString & KeyType,
  const QString & FingerprintSHA256, const QString & FingerprintMD5, HostKeyState State)
{
  Q_UNUSED(FingerprintMD5);
  Ask([=, this]() { emit hostKeyRequested(Host, Port, KeyType, FingerprintSHA256, (State == HostKeyChanged) ? 1 : 0); });
  QMutexLocker Lock(&FMutex);
  switch (FAnswer)
  {
    case 0: return HostKeyAccept;
    case 1: return HostKeyOnce;
    default: return HostKeyCancel;
  }
}

bool MobileUserInterface::PromptUser(PromptKind Kind, const QString & Name, const QString & Instructions, QList<Prompt> & Prompts)
{
  QVariantList List;
  for (const Prompt & P : Prompts)
  {
    List.append(QVariantMap{{QStringLiteral("text"), P.Text}, {QStringLiteral("echo"), P.Echo}});
  }
  Ask([=, this]() { emit promptRequested(static_cast<int>(Kind), Name, Instructions, List); });
  QMutexLocker Lock(&FMutex);
  if (!FPromptOk)
  {
    return false;
  }
  for (qsizetype I = 0; I < Prompts.size(); I++)
  {
    Prompts[I].Result = FValues.value(I);
  }
  return true;
}

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

unsigned int MobileUserInterface::QueryUser(const QString & Query, const QString & MoreMessages, unsigned int Answers)
{
  QVariantList Buttons = QueryButtons(Answers);
  Ask([=, this]() { emit queryRequested(Query, MoreMessages, Buttons); });
  QMutexLocker Lock(&FMutex);
  if ((FAnswer > 0) && (static_cast<unsigned int>(FAnswer) & Answers))
  {
    return static_cast<unsigned int>(FAnswer);
  }
  // dismissed: the safest answer offered
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

void MobileUserInterface::answerHostKey(int Answer)
{
  QMutexLocker Lock(&FMutex);
  FAnswer = Answer;
  FHasAnswer = true;
  FAnswered.wakeAll();
}

void MobileUserInterface::answerPrompt(bool Ok, const QStringList & Values)
{
  QMutexLocker Lock(&FMutex);
  FPromptOk = Ok;
  FValues = Values;
  FHasAnswer = true;
  FAnswered.wakeAll();
}

void MobileUserInterface::answerQuery(int Answer)
{
  QMutexLocker Lock(&FMutex);
  FAnswer = Answer;
  FHasAnswer = true;
  FAnswered.wakeAll();
}
