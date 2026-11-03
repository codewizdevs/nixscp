#include "mobile/AndroidFiles.h"

#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QUrl>

#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QJniEnvironment>
#include <QJniObject>

namespace
{
const jint FlagGrantReadUriPermission = 0x00000001;

QJniObject Context()
{
  return QNativeInterface::QAndroidApplication::context();
}

// content://<package>.qtprovider/... for a file under the provider's paths
QJniObject ContentUri(const QString & Path)
{
  QJniObject Ctx = Context();
  QJniObject Package = Ctx.callObjectMethod("getPackageName", "()Ljava/lang/String;");
  QJniObject Authority = QJniObject::fromString(Package.toString() + QStringLiteral(".qtprovider"));
  QJniObject File("java/io/File", "(Ljava/lang/String;)V", QJniObject::fromString(Path).object<jstring>());
  QJniObject Uri = QJniObject::callStaticObjectMethod("androidx/core/content/FileProvider", "getUriForFile",
    "(Landroid/content/Context;Ljava/lang/String;Ljava/io/File;)Landroid/net/Uri;",
    Ctx.object(), Authority.object<jstring>(), File.object());
  QJniEnvironment Env;
  if (Env.checkAndClearExceptions())
  {
    return QJniObject();
  }
  return Uri;
}

QString MimeType(const QString & Path)
{
  QString Type = QMimeDatabase().mimeTypeForFile(Path).name();
  return Type.isEmpty() ? QStringLiteral("application/octet-stream") : Type;
}

bool Start(const QJniObject & Intent, const QString & Title)
{
  QJniObject Chooser = QJniObject::callStaticObjectMethod("android/content/Intent", "createChooser",
    "(Landroid/content/Intent;Ljava/lang/CharSequence;)Landroid/content/Intent;",
    Intent.object(), QJniObject::fromString(Title).object<jstring>());
  Context().callMethod<void>("startActivity", "(Landroid/content/Intent;)V", Chooser.object());
  QJniEnvironment Env;
  return !Env.checkAndClearExceptions();
}
}
#endif

bool AndroidFiles::open(const QString & Path)
{
#ifdef Q_OS_ANDROID
  QJniObject Uri = ContentUri(Path);
  if (!Uri.isValid())
  {
    return false;
  }
  QJniObject Intent("android/content/Intent", "(Ljava/lang/String;)V",
    QJniObject::fromString(QStringLiteral("android.intent.action.VIEW")).object<jstring>());
  Intent.callObjectMethod("setDataAndType", "(Landroid/net/Uri;Ljava/lang/String;)Landroid/content/Intent;",
    Uri.object(), QJniObject::fromString(MimeType(Path)).object<jstring>());
  Intent.callObjectMethod("addFlags", "(I)Landroid/content/Intent;", FlagGrantReadUriPermission);
  // nothing installed for this type: the chooser would be empty
  QJniObject Manager = Context().callObjectMethod("getPackageManager", "()Landroid/content/pm/PackageManager;");
  QJniObject Handler = Intent.callObjectMethod("resolveActivity",
    "(Landroid/content/pm/PackageManager;)Landroid/content/ComponentName;", Manager.object());
  if (!Handler.isValid())
  {
    return false;
  }
  return Start(Intent, QFileInfo(Path).fileName());
#else
  return QDesktopServices::openUrl(QUrl::fromLocalFile(Path));
#endif
}

void AndroidFiles::keepAlive(bool Busy, const QString & Text)
{
#ifdef Q_OS_ANDROID
  QJniObject Ctx = Context();
  if (Busy)
  {
    if (!FAskedNotifications && (QNativeInterface::QAndroidApplication::sdkVersion() >= 33))
    {
      // the transfer notification (Android 13+ asks once)
      FAskedNotifications = true;
      QJniEnvironment Env;
      jclass StringClass = Env->FindClass("java/lang/String");
      jobjectArray Permissions = Env->NewObjectArray(1, StringClass, QJniObject::fromString(
        QStringLiteral("android.permission.POST_NOTIFICATIONS")).object<jstring>());
      Ctx.callMethod<void>("requestPermissions", "([Ljava/lang/String;I)V", Permissions, jint(1));
      Env.checkAndClearExceptions();
    }
    QJniObject::callStaticMethod<void>("com/nixscp/android/TransferService", "start",
      "(Landroid/content/Context;Ljava/lang/String;)V", Ctx.object(), QJniObject::fromString(Text).object<jstring>());
    FKeptAlive = true;
  }
  else if (FKeptAlive)
  {
    QJniObject::callStaticMethod<void>("com/nixscp/android/TransferService", "stop",
      "(Landroid/content/Context;)V", Ctx.object());
    FKeptAlive = false;
  }
  QJniEnvironment Env;
  Env.checkAndClearExceptions();
#else
  Q_UNUSED(Busy);
  Q_UNUSED(Text);
#endif
}

QStringList AndroidFiles::publish(const QStringList & Paths)
{
  QStringList Result;
#ifdef Q_OS_ANDROID
  QJniObject Ctx = Context();
  for (const QString & Path : Paths)
  {
    QJniObject Where = QJniObject::callStaticObjectMethod("com/nixscp/android/PublicDownloads", "save",
      "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", Ctx.object(),
      QJniObject::fromString(Path).object<jstring>(), QJniObject::fromString(MimeType(Path)).object<jstring>());
    QJniEnvironment Env;
    Env.checkAndClearExceptions();
    QString Place = Where.isValid() ? Where.toString() : QString();
    if (!Place.isEmpty())
    {
      QFile::remove(Path);   // not kept twice
    }
    Result.append(Place);
  }
#else
  Result = Paths;
#endif
  return Result;
}

bool AndroidFiles::share(const QStringList & Paths)
{
#ifdef Q_OS_ANDROID
  if (Paths.isEmpty())
  {
    return false;
  }
  bool Many = Paths.size() > 1;
  QJniObject Intent("android/content/Intent", "(Ljava/lang/String;)V",
    QJniObject::fromString(Many ? QStringLiteral("android.intent.action.SEND_MULTIPLE") : QStringLiteral("android.intent.action.SEND")).object<jstring>());
  QJniObject Extra = QJniObject::fromString(QStringLiteral("android.intent.extra.STREAM"));
  if (Many)
  {
    QJniObject List("java/util/ArrayList");
    for (const QString & Path : Paths)
    {
      QJniObject Uri = ContentUri(Path);
      if (Uri.isValid())
      {
        List.callMethod<jboolean>("add", "(Ljava/lang/Object;)Z", Uri.object());
      }
    }
    Intent.callObjectMethod("putParcelableArrayListExtra", "(Ljava/lang/String;Ljava/util/ArrayList;)Landroid/content/Intent;",
      Extra.object<jstring>(), List.object());
    Intent.callObjectMethod("setType", "(Ljava/lang/String;)Landroid/content/Intent;",
      QJniObject::fromString(QStringLiteral("*/*")).object<jstring>());
  }
  else
  {
    QJniObject Uri = ContentUri(Paths.first());
    if (!Uri.isValid())
    {
      return false;
    }
    Intent.callObjectMethod("putExtra", "(Ljava/lang/String;Landroid/os/Parcelable;)Landroid/content/Intent;",
      Extra.object<jstring>(), Uri.object());
    Intent.callObjectMethod("setType", "(Ljava/lang/String;)Landroid/content/Intent;",
      QJniObject::fromString(MimeType(Paths.first())).object<jstring>());
  }
  Intent.callObjectMethod("addFlags", "(I)Landroid/content/Intent;", FlagGrantReadUriPermission);
  return Start(Intent, QStringLiteral("Share"));
#else
  Q_UNUSED(Paths);
  return false;
#endif
}
