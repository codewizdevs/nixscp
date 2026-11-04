// The lock of NixSCP Pro's encrypted configuration on the phone. At start
// an encrypted configuration is opened with the NixSCP password (or the
// recovery key, or a fingerprint when turned on) before anything is loaded;
// then the application finishes its start. After some minutes in the
// background the app is covered again (transfers go on, the key stays in
// memory) until it is unlocked once more.
//
// Fingerprint unlock keeps the NixSCP password encrypted with an Android
// Keystore key that only a fingerprint releases (BiometricHelper.java).
#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QSettings>

#include <functional>

class AppLock : public QObject
{
  Q_OBJECT
  // the app is covered by the lock page
  Q_PROPERTY(bool locked READ locked NOTIFY changed)
  // the configuration is loaded (the first unlock happened)
  Q_PROPERTY(bool started READ started NOTIFY changed)
  // the configuration is encrypted (NixSCP Pro): the lock applies
  Q_PROPERTY(bool encrypted READ encrypted NOTIFY changed)
  Q_PROPERTY(bool biometricAvailable READ biometricAvailable NOTIFY changed)
  Q_PROPERTY(bool biometricEnabled READ biometricEnabled NOTIFY changed)
  // minutes in the background before locking again, 0 = only at start
  Q_PROPERTY(int autoLockMinutes READ autoLockMinutes WRITE setAutoLockMinutes NOTIFY changed)

public:
  // Startup: loads the configuration and starts what needs it
  AppLock(const QString & ConfigFile, std::function<void()> Startup, QObject * Parent = nullptr);

  // At start: locked when the configuration is encrypted, else started
  void Begin();
  bool locked() const { return FLocked; }
  bool started() const { return FStarted; }
  bool encrypted() const;
  bool biometricAvailable() const;
  bool biometricEnabled() const;
  int autoLockMinutes() const;
  void setAutoLockMinutes(int Minutes);

  // "" when unlocked, otherwise why not
  Q_INVOKABLE QString unlock(const QString & Secret, bool IsRecoveryKey);
  // the fingerprint prompt; biometricFinished() tells how it went
  Q_INVOKABLE void unlockWithBiometric();
  // checks the password and stores it for fingerprint unlock (biometricFinished)
  Q_INVOKABLE void enableBiometric(const QString & Password);
  Q_INVOKABLE void disableBiometric();
  Q_INVOKABLE void lockNow();
  // the app went to the background / came back (auto-lock)
  Q_INVOKABLE void background();
  Q_INVOKABLE void foreground();

  // BiometricHelper.onResult (JNI)
  void BiometricResult(int Request, bool Ok, const QString & Data, const QString & Error);

signals:
  void changed();
  // ok: unlocked (unlock) or turned on (enable); error: why not ("" = cancelled)
  void biometricFinished(bool ok, const QString & error);

private:
  QString FConfigFile;
  std::function<void()> FStartup;
  QSettings FSettings;
  bool FLocked = false;
  bool FStarted = false;
  bool FAway = false;
  QElapsedTimer FAwaySince;
  int FRequest = 0;
  enum { None, Enabling, Unlocking } FPending = None;

  void Finish();
};
