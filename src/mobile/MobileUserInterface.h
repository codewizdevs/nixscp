// The core's questions (host key, password, passphrase, queries) for the
// touch interface: each becomes a signal that QML shows as a sheet; the
// asking thread (a Terminal's worker) waits until QML answers. Asked on the
// GUI thread itself, a local event loop waits instead.
#pragma once

#include "core/FileSystem.h"

#include <QMutex>
#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <QWaitCondition>

class MobileUserInterface : public QObject, public UserInterface
{
  Q_OBJECT

public:
  explicit MobileUserInterface(QObject * Parent = nullptr);

  HostKeyAnswer VerifyHostKey(const QString & Host, int Port, const QString & KeyType,
    const QString & FingerprintSHA256, const QString & FingerprintMD5, HostKeyState State) override;
  bool PromptUser(PromptKind Kind, const QString & Name, const QString & Instructions, QList<Prompt> & Prompts) override;
  void DisplayBanner(const QString & SessionName, const QString & Banner) override;
  unsigned int QueryUser(const QString & Query, const QString & MoreMessages, unsigned int Answers) override;

  // Answers from QML (one question is open at a time)
  // 0 = accept and remember, 1 = accept once, 2 = cancel
  Q_INVOKABLE void answerHostKey(int Answer);
  Q_INVOKABLE void answerPrompt(bool Ok, const QStringList & Values);
  // one of the qa* values offered, 0 = dismissed
  Q_INVOKABLE void answerQuery(int Answer);

  // Offered answers of a query as [{answer, caption}], positive first
  static QVariantList QueryButtons(unsigned int Answers);

signals:
  // State: 0 unknown host, 1 changed key (possible attack)
  void hostKeyRequested(const QString & host, int port, const QString & keyType, const QString & fingerprint, int state);
  // Kind: 0 password, 1 passphrase, 2 keyboard-interactive, 3 user name, 4 file name;
  // prompts: [{text, echo}]
  void promptRequested(int kind, const QString & name, const QString & instructions, const QVariantList & prompts);
  void queryRequested(const QString & query, const QString & moreMessages, const QVariantList & buttons);
  void bannerShown(const QString & sessionName, const QString & banner);

private:
  QMutex FMutex;
  QWaitCondition FAnswered;
  bool FHasAnswer = false;
  int FAnswer = 0;
  bool FPromptOk = false;
  QStringList FValues;

  // Emits the request on the GUI thread and waits for the answer
  void Ask(const std::function<void()> & Emit);
};
