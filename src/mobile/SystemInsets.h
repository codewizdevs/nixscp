// The space taken by the status bar and the navigation bar (Android 15 draws
// apps edge to edge), in device-independent pixels, for the QML layout.
// Qt 6.8 has no QML safe area yet; on Android the window insets are read
// through JNI. Zero elsewhere.
#pragma once

#include <QObject>

class SystemInsets : public QObject
{
  Q_OBJECT
  Q_PROPERTY(int top READ top NOTIFY changed)
  Q_PROPERTY(int bottom READ bottom NOTIFY changed)
  // the on-screen keyboard's height over the navigation bar (0 = hidden);
  // Qt's keyboardRectangle is not reliable with adjustNothing
  Q_PROPERTY(int keyboard READ keyboard NOTIFY changed)

public:
  explicit SystemInsets(QObject * Parent = nullptr);
  int top() const { return FTop; }
  int bottom() const { return FBottom; }
  int keyboard() const { return FKeyboard; }
  // Reads them again (after the window was shown or resized)
  Q_INVOKABLE void update();
  // With the window not resized for the keyboard (adjustNothing), Qt does
  // not notice when the keyboard is closed with Back: it still counts it
  // as open and spends the next Back on closing it. This asks Android
  // whether the keyboard is shown (Android 11+) and tells Qt when it is not;
  // it also updates keyboard().
  Q_INVOKABLE void checkKeyboard();

signals:
  void changed();

private:
  int FTop = 0;
  int FBottom = 0;
  int FKeyboard = 0;
  bool FKeyboardSeen = false;
  int FRetries = 0;
};
