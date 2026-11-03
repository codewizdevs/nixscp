#include "mobile/SystemInsets.h"

#include <QGuiApplication>
#include <QScreen>
#include <QTimer>

#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QJniObject>
#endif

SystemInsets::SystemInsets(QObject * Parent) : QObject(Parent)
{
  // the decor view has its insets once the window is attached
  QTimer::singleShot(0, this, &SystemInsets::update);
  QTimer::singleShot(500, this, &SystemInsets::update);
}

void SystemInsets::update()
{
#ifdef Q_OS_ANDROID
  QNativeInterface::QAndroidApplication::runOnAndroidMainThread([this]()
  {
    QJniObject Activity = QNativeInterface::QAndroidApplication::context();
    QJniObject Window = Activity.callObjectMethod("getWindow", "()Landroid/view/Window;");
    QJniObject Decor = Window.isValid() ? Window.callObjectMethod("getDecorView", "()Landroid/view/View;") : QJniObject();
    QJniObject Insets = Decor.isValid() ? Decor.callObjectMethod("getRootWindowInsets", "()Landroid/view/WindowInsets;") : QJniObject();
    if (!Insets.isValid())
    {
      return;
    }
    jint Types = QJniObject::callStaticMethod<jint>("android/view/WindowInsets$Type", "systemBars", "()I");
    QJniObject Bars = Insets.callObjectMethod("getInsets", "(I)Landroid/graphics/Insets;", Types);
    if (!Bars.isValid())
    {
      return;
    }
    int TopPx = Bars.getField<jint>("top");
    int BottomPx = Bars.getField<jint>("bottom");
    QMetaObject::invokeMethod(this, [this, TopPx, BottomPx]()
    {
      qreal Ratio = (QGuiApplication::primaryScreen() != nullptr) ? QGuiApplication::primaryScreen()->devicePixelRatio() : 1.0;
      int Top = qRound(TopPx / Ratio);
      int Bottom = qRound(BottomPx / Ratio);
      if ((Top != FTop) || (Bottom != FBottom))
      {
        FTop = Top;
        FBottom = Bottom;
        emit changed();
      }
    }, Qt::QueuedConnection);
  });
#endif
}
