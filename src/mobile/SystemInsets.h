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

public:
  explicit SystemInsets(QObject * Parent = nullptr);
  int top() const { return FTop; }
  int bottom() const { return FBottom; }
  // Reads them again (after the window was shown or resized)
  Q_INVOKABLE void update();

signals:
  void changed();

private:
  int FTop = 0;
  int FBottom = 0;
};
