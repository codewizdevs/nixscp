#include "mobile/SystemInsets.h"

#include <QGuiApplication>
#include <QInputMethod>
#include <QScreen>
#include <QTimer>

#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QJniEnvironment>
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
    QJniEnvironment Env;
    QJniObject Activity = QNativeInterface::QAndroidApplication::context();
    QJniObject Window = Activity.callObjectMethod("getWindow", "()Landroid/view/Window;");
    QJniObject Decor = Window.isValid() ? Window.callObjectMethod("getDecorView", "()Landroid/view/View;") : QJniObject();
    QJniObject Insets = Decor.isValid() ? Decor.callObjectMethod("getRootWindowInsets", "()Landroid/view/WindowInsets;") : QJniObject();
    int TopPx = -1;
    int BottomPx = -1;
    if (Insets.isValid())
    {
      if (QNativeInterface::QAndroidApplication::sdkVersion() >= 30)
      {
        // the bars' space even while they are hidden for a moment (an app
        // switch, folding): read then, getInsets() gives 0 and the pages
        // would stay under the status bar; the camera cutout too
        jint Types = QJniObject::callStaticMethod<jint>("android/view/WindowInsets$Type", "systemBars", "()I") |
          QJniObject::callStaticMethod<jint>("android/view/WindowInsets$Type", "displayCutout", "()I");
        QJniObject Bars = Insets.callObjectMethod("getInsetsIgnoringVisibility", "(I)Landroid/graphics/Insets;", Types);
        if (!Env.checkAndClearExceptions() && Bars.isValid())
        {
          TopPx = Bars.getField<jint>("top");
          BottomPx = Bars.getField<jint>("bottom");
        }
      }
      else
      {
        TopPx = Insets.callMethod<jint>("getSystemWindowInsetTop");
        BottomPx = Insets.callMethod<jint>("getSystemWindowInsetBottom");
      }
      Env.checkAndClearExceptions();
    }
    QMetaObject::invokeMethod(this, [this, TopPx, BottomPx]()
    {
      // not known yet (the window is not attached): again a bit later
      if (TopPx <= 0)
      {
        if (FRetries++ < 20)
        {
          QTimer::singleShot(250, this, &SystemInsets::update);
        }
        return;
      }
      FRetries = 0;
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

void SystemInsets::checkKeyboard()
{
#ifdef Q_OS_ANDROID
  if (QNativeInterface::QAndroidApplication::sdkVersion() < 30)
  {
    return;
  }
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
    jint Ime = QJniObject::callStaticMethod<jint>("android/view/WindowInsets$Type", "ime", "()I");
    jint Bars = QJniObject::callStaticMethod<jint>("android/view/WindowInsets$Type", "navigationBars", "()I");
    bool Shown = Insets.callMethod<jboolean>("isVisible", "(I)Z", Ime);
    QJniObject ImeInsets = Insets.callObjectMethod("getInsets", "(I)Landroid/graphics/Insets;", Ime);
    QJniObject BarInsets = Insets.callObjectMethod("getInsets", "(I)Landroid/graphics/Insets;", Bars);
    int ImePx = (Shown && ImeInsets.isValid()) ? ImeInsets.getField<jint>("bottom") : 0;
    int BarPx = BarInsets.isValid() ? BarInsets.getField<jint>("bottom") : 0;
    QMetaObject::invokeMethod(this, [this, Shown, ImePx, BarPx]()
    {
      qreal Ratio = (QGuiApplication::primaryScreen() != nullptr) ? QGuiApplication::primaryScreen()->devicePixelRatio() : 1.0;
      int Keyboard = qMax(0, qRound((ImePx - BarPx) / Ratio));
      if (Keyboard != FKeyboard)
      {
        FKeyboard = Keyboard;
        emit changed();
      }
      // closed after it was seen open (not while it is still coming up)
      if (Shown)
      {
        FKeyboardSeen = true;
      }
      else if (FKeyboardSeen)
      {
        FKeyboardSeen = false;
        if (QGuiApplication::inputMethod()->isVisible())
        {
          QGuiApplication::inputMethod()->hide();
        }
      }
    }, Qt::QueuedConnection);
  });
#endif
}
