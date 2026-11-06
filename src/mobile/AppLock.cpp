#include "mobile/AppLock.h"

#include "core/ConfigVault.h"

#include <QDateTime>
#include <QFile>
#include <QStandardPaths>

#include <time.h>

#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QJniEnvironment>
#include <QJniObject>

namespace
{
AppLock * Instance = nullptr;

void JNICALL OnResult(JNIEnv *, jclass, jint Request, jboolean Ok, jstring Data, jstring Error)
{
  QString DataText = Data ? QJniObject(Data).toString() : QString();
  QString ErrorText = Error ? QJniObject(Error).toString() : QString();
  if (Instance != nullptr)
  {
    QMetaObject::invokeMethod(Instance, [Request, Ok, DataText, ErrorText]()
    {
      if (Instance != nullptr)
      {
        Instance->BiometricResult(Request, Ok, DataText, ErrorText);
      }
    }, Qt::QueuedConnection);
  }
}

QJniObject Activity()
{
  return QNativeInterface::QAndroidApplication::context();
}
}
#endif

AppLock::AppLock(const QString & ConfigFile, std::function<void()> Startup, QObject * Parent) :
  QObject(Parent), FConfigFile(ConfigFile), FStartup(std::move(Startup)),
  FSettings(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/mobile.ini"),
    QSettings::IniFormat)
{
#ifdef Q_OS_ANDROID
  Instance = this;
  QJniEnvironment Env;
  static const JNINativeMethod Methods[] = {
    {"onResult", "(IZLjava/lang/String;Ljava/lang/String;)V", reinterpret_cast<void *>(&OnResult)}};
  Env.registerNativeMethods("com/nixscp/android/BiometricHelper", Methods, 1);
#endif
}

void AppLock::Begin()
{
  FProblem = ConfigVault::ConfigProblem(FConfigFile);
  if (!FProblem.isEmpty() || ConfigVault::IsSealedFile(FConfigFile))
  {
    FLocked = true;
    emit changed();
  }
  else
  {
    Finish();
  }
}

void AppLock::startOver()
{
  if (FProblem.isEmpty())
  {
    return;
  }
  const QStringList Files = ConfigVault::StartOverFiles(FConfigFile);
  for (const QString & File : Files)
  {
    QFile::remove(File);
  }
  FProblem.clear();
  Finish();
}

void AppLock::Finish()
{
  FLocked = false;
  if (!FStarted)
  {
    FStarted = true;
    FStartup();
  }
  emit changed();
}

bool AppLock::encrypted() const
{
  return ConfigVault::Enabled() || ConfigVault::IsSealedFile(FConfigFile);
}

bool AppLock::biometricAvailable() const
{
#ifdef Q_OS_ANDROID
  QJniEnvironment Env;
  bool Available = QJniObject::callStaticMethod<jboolean>("com/nixscp/android/BiometricHelper", "available",
    "(Landroid/content/Context;)Z", Activity().object());
  return !Env.checkAndClearExceptions() && Available;
#else
  return false;
#endif
}

bool AppLock::biometricEnabled() const
{
  return !FSettings.value(QStringLiteral("Lock/Biometric")).toString().isEmpty();
}

int AppLock::autoLockMinutes() const
{
  return FSettings.value(QStringLiteral("Lock/AutoLockMinutes"), 5).toInt();
}

void AppLock::setAutoLockMinutes(int Minutes)
{
  FSettings.setValue(QStringLiteral("Lock/AutoLockMinutes"), qMax(0, Minutes));
  FSettings.sync();
  emit changed();
}

QString AppLock::unlock(const QString & Secret, bool IsRecoveryKey)
{
  if (Secret.isEmpty())
  {
    return QStringLiteral("Enter the password.");
  }
  QString Error;
  // the same check after an auto-lock: the key of the file must open
  if (!ConfigVault::Unlock(FConfigFile, Secret, IsRecoveryKey, &Error))
  {
    return Error.isEmpty() ? QStringLiteral("Wrong password.") : Error;
  }
  Finish();
  return QString();
}

void AppLock::unlockWithBiometric()
{
#ifdef Q_OS_ANDROID
  QString Stored = FSettings.value(QStringLiteral("Lock/Biometric")).toString();
  if (Stored.isEmpty())
  {
    emit biometricFinished(false, QStringLiteral("Fingerprint unlock is not turned on."));
    return;
  }
  FPending = Unlocking;
  QJniObject::callStaticMethod<void>("com/nixscp/android/BiometricHelper", "decrypt",
    "(Landroid/app/Activity;ILjava/lang/String;)V", Activity().object(), jint(++FRequest),
    QJniObject::fromString(Stored).object<jstring>());
#endif
}

void AppLock::enableBiometric(const QString & Password)
{
  // only the right password is stored
  QString Error;
  if (!ConfigVault::Unlock(FConfigFile, Password, false, &Error))
  {
    emit biometricFinished(false, Error.isEmpty() ? QStringLiteral("Wrong password.") : Error);
    return;
  }
#ifdef Q_OS_ANDROID
  FPending = Enabling;
  QJniObject::callStaticMethod<void>("com/nixscp/android/BiometricHelper", "encrypt",
    "(Landroid/app/Activity;ILjava/lang/String;)V", Activity().object(), jint(++FRequest),
    QJniObject::fromString(Password).object<jstring>());
#endif
}

void AppLock::disableBiometric()
{
  FSettings.remove(QStringLiteral("Lock/Biometric"));
  FSettings.sync();
#ifdef Q_OS_ANDROID
  QJniObject::callStaticMethod<void>("com/nixscp/android/BiometricHelper", "forget");
#endif
  emit changed();
}

void AppLock::BiometricResult(int Request, bool Ok, const QString & Data, const QString & Error)
{
  if (Request != FRequest)
  {
    return;
  }
  auto Pending = FPending;
  FPending = None;
  if (!Ok)
  {
    if (Error == QStringLiteral("invalidated"))
    {
      // a fingerprint was added or removed on the phone: Android dropped the key
      disableBiometric();
      emit biometricFinished(false, QStringLiteral("The phone's fingerprints changed, so fingerprint unlock was turned off. "
        "Unlock with the password and turn it on again in NixSCP Pro."));
      return;
    }
    emit biometricFinished(false, (Error == QStringLiteral("cancelled")) ? QString() : Error);
    return;
  }
  if (Pending == Enabling)
  {
    FSettings.setValue(QStringLiteral("Lock/Biometric"), Data);
    FSettings.sync();
    emit changed();
    emit biometricFinished(true, QString());
    return;
  }
  QString Problem = unlock(Data, false);
  if (!Problem.isEmpty())
  {
    // the password was changed (on another device): the stored one is old
    disableBiometric();
    emit biometricFinished(false, QStringLiteral("The NixSCP password has changed. Unlock with the new password, then "
      "turn fingerprint unlock on again."));
    return;
  }
  emit biometricFinished(true, QString());
}

void AppLock::lockNow()
{
  if (encrypted() && FStarted)
  {
    FLocked = true;
    emit changed();
  }
}

void AppLock::background()
{
  if (!FAway)
  {
    FAway = true;
    FAwaySince = BootTime();
  }
}

void AppLock::foreground()
{
  if (!FAway)
  {
    return;
  }
  FAway = false;
  int Minutes = autoLockMinutes();
  if ((Minutes > 0) && FStarted && encrypted() && (BootTime() - FAwaySince >= qint64(Minutes) * 60 * 1000))
  {
    lockNow();
  }
}

qint64 AppLock::BootTime()
{
#ifdef CLOCK_BOOTTIME
  timespec Now{};
  if (clock_gettime(CLOCK_BOOTTIME, &Now) == 0)
  {
    return qint64(Now.tv_sec) * 1000 + Now.tv_nsec / 1000000;
  }
#endif
  return QDateTime::currentMSecsSinceEpoch();
}
