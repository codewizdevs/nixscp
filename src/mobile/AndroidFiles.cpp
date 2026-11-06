#include "mobile/AndroidFiles.h"
#include "mobile/PrivateState.h"
#include "mobile/TextFile.h"

#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QStandardPaths>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QVariantMap>
#include <QPointer>
#include <QUrl>

#include <QDateTime>

#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QJniEnvironment>
#include <QJniObject>
#include <QtCore/private/qjnihelpers_p.h>

namespace
{
const jint FlagGrantReadUriPermission = 0x00000001;
const jint FlagGrantWriteUriPermission = 0x00000002;

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

// what editors claim: text/x-php, application/x-shellscript... are text/plain to them
QString EditMimeType(const QString & Path)
{
  QMimeType Type = QMimeDatabase().mimeTypeForFile(Path);
  return (Type.inherits(QStringLiteral("text/plain")) || Type.name().startsWith(QStringLiteral("text/")))
    ? QStringLiteral("text/plain") : MimeType(Path);
}

QJniObject IntentFor(const QString & Action, const QJniObject & Uri, const QString & Type)
{
  QJniObject Intent("android/content/Intent", "(Ljava/lang/String;)V", QJniObject::fromString(Action).object<jstring>());
  Intent.callObjectMethod("setDataAndType", "(Landroid/net/Uri;Ljava/lang/String;)Landroid/content/Intent;",
    Uri.object(), QJniObject::fromString(Type).object<jstring>());
  return Intent;
}

bool StartChooser(const QJniObject & Intent, const QString & Title)
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
  return StartChooser(Intent, QFileInfo(Path).fileName());
#else
  return QDesktopServices::openUrl(QUrl::fromLocalFile(Path));
#endif
}

void AndroidFiles::keepAlive(bool Busy, const QString & Text, int Percent, const QString & Who)
{
  if (Busy)
  {
    FKeepers.insert(Who, Keeper{Text, Percent});
  }
  else
  {
    FKeepers.remove(Who);
  }
#ifdef Q_OS_ANDROID
  QJniObject Ctx = Context();
  if (!FKeepers.isEmpty())
  {
    // the transfers' progress before anything else
    const Keeper Shown = FKeepers.value(QStringLiteral("transfers"), FKeepers.first());
    // the notification at most about once a second (a new text at once)
    qint64 Now = QDateTime::currentMSecsSinceEpoch();
    if (FKeptAlive && (Shown.Text == FLastText) && ((Shown.Percent == FLastPercent) || (Now - FLastUpdate < 1000)))
    {
      return;
    }
    FLastText = Shown.Text;
    FLastPercent = Shown.Percent;
    FLastUpdate = Now;
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
      "(Landroid/content/Context;Ljava/lang/String;I)V", Ctx.object(), QJniObject::fromString(Shown.Text).object<jstring>(),
      jint(Shown.Percent));
    FKeptAlive = true;
  }
  else if (FKeptAlive)
  {
    QJniObject::callStaticMethod<void>("com/nixscp/android/TransferService", "stop",
      "(Landroid/content/Context;)V", Ctx.object());
    FKeptAlive = false;
    FLastText.clear();
    FLastPercent = -2;
  }
  QJniEnvironment Env;
  Env.checkAndClearExceptions();
#endif
}

QList<QPair<QString, QString>> AndroidFiles::FilesToPublish(const QString & Path)
{
  QFileInfo Info(Path);
  if (!Info.isDir())
  {
    return {{Path, QString()}};
  }
  QList<QPair<QString, QString>> Result;
  QDir Parent = Info.dir();
  QDirIterator It(Path, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
  while (It.hasNext())
  {
    It.next();
    Result.append({It.filePath(), Parent.relativeFilePath(It.fileInfo().absolutePath())});
  }
  return Result;
}

QStringList AndroidFiles::publish(const QStringList & Paths)
{
  QStringList Result;
#ifdef Q_OS_ANDROID
  QJniObject Ctx = Context();
  for (const QString & Path : Paths)
  {
    // a folder goes file by file (MediaStore has no folders of its own)
    QString Place;
    bool All = true;
    const QList<QPair<QString, QString>> Files = FilesToPublish(Path);
    for (const auto & [File, Folder] : Files)
    {
      QJniObject Where = QJniObject::callStaticObjectMethod("com/nixscp/android/PublicDownloads", "save",
        "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", Ctx.object(),
        QJniObject::fromString(File).object<jstring>(), QJniObject::fromString(MimeType(File)).object<jstring>(),
        QJniObject::fromString(Folder).object<jstring>());
      QJniEnvironment Env;
      Env.checkAndClearExceptions();
      QString Saved = Where.isValid() ? Where.toString() : QString();
      if (Saved.isEmpty())
      {
        All = false;
      }
      else
      {
        QFile::remove(File);   // not kept twice
        Place = Saved;
      }
    }
    if (QFileInfo(Path).isDir())
    {
      if (All)
      {
        QDir(Path).removeRecursively();
      }
      Place = All ? (QStringLiteral("Download/NixSCP/") + QFileInfo(Path).fileName()) : QString();
    }
    Result.append(Place);
  }
#else
  Result = Paths;
#endif
  return Result;
}

QString AndroidFiles::saveCopy(const QString & Name, const QString & Text, const QString & Encoding)
{
  QByteArray Data;
  if (!TextFile::Encode(Text, Encoding, Data) && !TextFile::Encode(Text, QLatin1String(TextFile::Utf8), Data))
  {
    return QString();
  }
  // session 0: nobody's, gone at the next start at the latest
  QString Folder = MobileCache::Folder(QStringLiteral("edit"), 0) + QStringLiteral("/copy");
  QDir().mkpath(Folder);
  QString Path = Folder + QLatin1Char('/') + Name;
  QFile File(Path);
  if (!File.open(QIODevice::WriteOnly) || (File.write(Data) != Data.size()) || !File.flush())
  {
    File.remove();
    return QString();
  }
  File.close();
  QString Place = publish({Path}).value(0);
#ifdef Q_OS_ANDROID
  QFile::remove(Path);
#endif
  return Place;
}

void AndroidFiles::SecureWindow()
{
#ifdef Q_OS_ANDROID
  QNativeInterface::QAndroidApplication::runOnAndroidMainThread([]()
  {
    const jint FlagSecure = 0x00002000;   // WindowManager.LayoutParams.FLAG_SECURE
    QJniEnvironment Env;
    QJniObject Window = Context().callObjectMethod("getWindow", "()Landroid/view/Window;");
    if (Window.isValid())
    {
      Window.callMethod<void>("setFlags", "(II)V", FlagSecure, FlagSecure);
    }
    Env.checkAndClearExceptions();
  });
#endif
}

bool AndroidFiles::edit(const QString & Path, const QString & Package)
{
#ifdef Q_OS_ANDROID
  QJniObject Uri = ContentUri(Path);
  if (!Uri.isValid())
  {
    return false;
  }
  QJniObject Manager = Context().callObjectMethod("getPackageManager", "()Landroid/content/pm/PackageManager;");
  QString Type = EditMimeType(Path);
  // EDIT where the app has it, VIEW (with write access) otherwise: many
  // editors only declare VIEW
  for (const QString & Action : {QStringLiteral("android.intent.action.EDIT"), QStringLiteral("android.intent.action.VIEW")})
  {
    QJniObject Intent = IntentFor(Action, Uri, Type);
    Intent.callObjectMethod("addFlags", "(I)Landroid/content/Intent;", FlagGrantReadUriPermission | FlagGrantWriteUriPermission);
    bool Ask = Package.isEmpty() || (Package == QStringLiteral("ask"));
    if (!Ask)
    {
      Intent.callObjectMethod("setPackage", "(Ljava/lang/String;)Landroid/content/Intent;",
        QJniObject::fromString(Package).object<jstring>());
    }
    QJniObject Handler = Intent.callObjectMethod("resolveActivity",
      "(Landroid/content/pm/PackageManager;)Landroid/content/ComponentName;", Manager.object());
    if (!Handler.isValid())
    {
      continue;
    }
    if (Ask)
    {
      return StartChooser(Intent, QFileInfo(Path).fileName());
    }
    const jint FlagNewTask = 0x10000000;
    Intent.callObjectMethod("addFlags", "(I)Landroid/content/Intent;", FlagNewTask);
    Context().callMethod<void>("startActivity", "(Landroid/content/Intent;)V", Intent.object());
    QJniEnvironment Env;
    return !Env.checkAndClearExceptions();
  }
  return false;
#else
  Q_UNUSED(Package);
  return open(Path);
#endif
}

QVariantList AndroidFiles::editors() const
{
  QVariantList Result;
#ifdef Q_OS_ANDROID
  QJniObject Manager = Context().callObjectMethod("getPackageManager", "()Landroid/content/pm/PackageManager;");
  QJniObject Uri = QJniObject::callStaticObjectMethod("android/net/Uri", "parse", "(Ljava/lang/String;)Landroid/net/Uri;",
    QJniObject::fromString(QStringLiteral("content://com.nixscp.android.qtprovider/edited/file.txt")).object<jstring>());
  QJniObject Own = Context().callObjectMethod("getPackageName", "()Ljava/lang/String;");
  QStringList Seen{Own.toString()};
  for (const QString & Action : {QStringLiteral("android.intent.action.EDIT"), QStringLiteral("android.intent.action.VIEW")})
  {
    QJniObject Intent = IntentFor(Action, Uri, QStringLiteral("text/plain"));
    QJniObject List = Manager.callObjectMethod("queryIntentActivities",
      "(Landroid/content/Intent;I)Ljava/util/List;", Intent.object(), jint(0));
    QJniEnvironment Env;
    if (Env.checkAndClearExceptions() || !List.isValid())
    {
      continue;
    }
    int Count = List.callMethod<jint>("size");
    for (int I = 0; I < Count; I++)
    {
      QJniObject Info = List.callObjectMethod("get", "(I)Ljava/lang/Object;", jint(I));
      QJniObject Activity = Info.getObjectField("activityInfo", "Landroid/content/pm/ActivityInfo;");
      QString Package = Activity.getObjectField<jstring>("packageName").toString();
      if (Package.isEmpty() || Seen.contains(Package))
      {
        continue;
      }
      Seen.append(Package);
      QString Label = Info.callObjectMethod("loadLabel", "(Landroid/content/pm/PackageManager;)Ljava/lang/CharSequence;",
        Manager.object()).callObjectMethod("toString", "()Ljava/lang/String;").toString();
      Result.append(QVariantMap{{QStringLiteral("name"), Label.isEmpty() ? Package : Label},
        {QStringLiteral("package"), Package}});
    }
  }
#endif
  return Result;
}

bool AndroidFiles::shareText(const QString & Text)
{
#ifdef Q_OS_ANDROID
  QJniObject Intent("android/content/Intent", "(Ljava/lang/String;)V",
    QJniObject::fromString(QStringLiteral("android.intent.action.SEND")).object<jstring>());
  Intent.callObjectMethod("setType", "(Ljava/lang/String;)Landroid/content/Intent;",
    QJniObject::fromString(QStringLiteral("text/plain")).object<jstring>());
  Intent.callObjectMethod("putExtra", "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;",
    QJniObject::fromString(QStringLiteral("android.intent.extra.TEXT")).object<jstring>(),
    QJniObject::fromString(Text).object<jstring>());
  return StartChooser(Intent, QStringLiteral("Share"));
#else
  Q_UNUSED(Text);
  return false;
#endif
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
  return StartChooser(Intent, QStringLiteral("Share"));
#else
  Q_UNUSED(Paths);
  return false;
#endif
}

QString AndroidFiles::StorageRoot()
{
#ifdef Q_OS_ANDROID
  QJniObject Dir = QJniObject::callStaticObjectMethod("android/os/Environment", "getExternalStorageDirectory",
    "()Ljava/io/File;");
  QJniEnvironment Env;
  if (!Env.checkAndClearExceptions() && Dir.isValid())
  {
    QString Path = Dir.callObjectMethod("getAbsolutePath", "()Ljava/lang/String;").toString();
    if (!Path.isEmpty())
    {
      return Path;
    }
  }
  return QStringLiteral("/storage/emulated/0");
#else
  return QDir::homePath();
#endif
}

bool AndroidFiles::HasStorageAccess()
{
#ifdef Q_OS_ANDROID
  QJniEnvironment Env;
  bool Result;
  if (QNativeInterface::QAndroidApplication::sdkVersion() >= 30)
  {
    Result = QJniObject::callStaticMethod<jboolean>("android/os/Environment", "isExternalStorageManager");
  }
  else
  {
    const jint Granted = 0;   // PackageManager.PERMISSION_GRANTED
    Result = Context().callMethod<jint>("checkSelfPermission", "(Ljava/lang/String;)I",
      QJniObject::fromString(QStringLiteral("android.permission.WRITE_EXTERNAL_STORAGE")).object<jstring>()) == Granted;
  }
  return !Env.checkAndClearExceptions() && Result;
#else
  return true;
#endif
}

void AndroidFiles::RequestStorageAccess()
{
#ifdef Q_OS_ANDROID
  QJniObject Ctx = Context();
  QJniEnvironment Env;
  if (QNativeInterface::QAndroidApplication::sdkVersion() >= 30)
  {
    // the app's own page of "All files access"; some devices only have the list
    QJniObject Package = Ctx.callObjectMethod("getPackageName", "()Ljava/lang/String;");
    QJniObject Uri = QJniObject::callStaticObjectMethod("android/net/Uri", "parse", "(Ljava/lang/String;)Landroid/net/Uri;",
      QJniObject::fromString(QStringLiteral("package:") + Package.toString()).object<jstring>());
    QJniObject Intent("android/content/Intent", "(Ljava/lang/String;Landroid/net/Uri;)V",
      QJniObject::fromString(QStringLiteral("android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION")).object<jstring>(),
      Uri.object());
    Ctx.callMethod<void>("startActivity", "(Landroid/content/Intent;)V", Intent.object());
    if (Env.checkAndClearExceptions())
    {
      QJniObject List("android/content/Intent", "(Ljava/lang/String;)V",
        QJniObject::fromString(QStringLiteral("android.settings.MANAGE_ALL_FILES_ACCESS_PERMISSION")).object<jstring>());
      Ctx.callMethod<void>("startActivity", "(Landroid/content/Intent;)V", List.object());
      Env.checkAndClearExceptions();
    }
  }
  else
  {
    jclass StringClass = Env->FindClass("java/lang/String");
    jobjectArray Permissions = Env->NewObjectArray(2, StringClass, nullptr);
    Env->SetObjectArrayElement(Permissions, 0, QJniObject::fromString(
      QStringLiteral("android.permission.READ_EXTERNAL_STORAGE")).object<jstring>());
    Env->SetObjectArrayElement(Permissions, 1, QJniObject::fromString(
      QStringLiteral("android.permission.WRITE_EXTERNAL_STORAGE")).object<jstring>());
    Ctx.callMethod<void>("requestPermissions", "([Ljava/lang/String;I)V", Permissions, jint(2));
    Env.checkAndClearExceptions();
  }
#endif
}

//---------------------------------------------------------------------------
// intents: Share -> NixSCP and the sites' home-screen shortcuts
namespace
{
const char * OpenSiteAction = "com.nixscp.android.OPEN_SITE";
const char * SiteExtra = "com.nixscp.android.SITE";
}

#ifdef Q_OS_ANDROID
namespace
{
// what an intent carries, read while its reference is valid
void ReadIntent(const QJniObject & Intent, QString & Action, QStringList & Uris, QString & Site)
{
  if (!Intent.isValid())
  {
    return;
  }
  QJniEnvironment Env;
  Action = Intent.callObjectMethod("getAction", "()Ljava/lang/String;").toString();
  QJniObject Stream = QJniObject::fromString(QStringLiteral("android.intent.extra.STREAM"));
  if (Action == QStringLiteral("android.intent.action.SEND"))
  {
    QJniObject Uri = Intent.callObjectMethod("getParcelableExtra", "(Ljava/lang/String;)Landroid/os/Parcelable;",
      Stream.object<jstring>());
    if (Uri.isValid())
    {
      Uris.append(Uri.callObjectMethod("toString", "()Ljava/lang/String;").toString());
    }
  }
  else if (Action == QStringLiteral("android.intent.action.SEND_MULTIPLE"))
  {
    QJniObject List = Intent.callObjectMethod("getParcelableArrayListExtra", "(Ljava/lang/String;)Ljava/util/ArrayList;",
      Stream.object<jstring>());
    int Count = List.isValid() ? List.callMethod<jint>("size") : 0;
    for (int I = 0; I < Count; I++)
    {
      QJniObject Uri = List.callObjectMethod("get", "(I)Ljava/lang/Object;", jint(I));
      if (Uri.isValid())
      {
        Uris.append(Uri.callObjectMethod("toString", "()Ljava/lang/String;").toString());
      }
    }
  }
  else if (Action == QLatin1String(OpenSiteAction))
  {
    Site = Intent.callObjectMethod("getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;",
      QJniObject::fromString(QLatin1String(SiteExtra)).object<jstring>()).toString();
  }
  Env.checkAndClearExceptions();
}
}

class AndroidFiles::IntentListener : public QtAndroidPrivate::NewIntentListener
{
public:
  explicit IntentListener(AndroidFiles * Owner) : FOwner(Owner) {}
  bool handleNewIntent(JNIEnv *, jobject Intent) override
  {
    QString Action;
    QStringList Uris;
    QString Site;
    ReadIntent(QJniObject(Intent), Action, Uris, Site);
    QPointer<AndroidFiles> Owner(FOwner);
    QMetaObject::invokeMethod(FOwner, [Owner, Action, Uris, Site]()
    {
      if (Owner != nullptr)
      {
        Owner->HandleIntent(Action, Uris, Site);
      }
    }, Qt::QueuedConnection);
    return true;
  }

private:
  AndroidFiles * FOwner;
};
#else
class AndroidFiles::IntentListener {};
#endif

AndroidFiles::AndroidFiles(QObject * Parent) : QObject(Parent)
{
}

AndroidFiles::~AndroidFiles()
{
#ifdef Q_OS_ANDROID
  if (FListener != nullptr)
  {
    QtAndroidPrivate::unregisterNewIntentListener(FListener);
  }
#endif
  delete FListener;
}

void AndroidFiles::Start()
{
#ifdef Q_OS_ANDROID
  FListener = new IntentListener(this);
  QtAndroidPrivate::registerNewIntentListener(FListener);
  QJniObject Activity = QNativeInterface::QAndroidApplication::context();
  QJniObject Intent = Activity.callObjectMethod("getIntent", "()Landroid/content/Intent;");
  QString Action;
  QStringList Uris;
  QString Site;
  ReadIntent(Intent, Action, Uris, Site);
  HandleIntent(Action, Uris, Site);
#endif
}

void AndroidFiles::HandleIntent(const QString & Action, const QStringList & Uris, const QString & Site)
{
  Q_UNUSED(Action);
  if (!Uris.isEmpty())
  {
    FShared = Uris;
    FSharedNames.clear();
    for (const QString & Uri : Uris)
    {
      FSharedNames.append(DisplayName(Uri));
    }
    emit sharedFilesChanged();
  }
  if (!Site.isEmpty())
  {
    emit openSiteRequested(Site);
  }
}

void AndroidFiles::clearShared()
{
  if (!FShared.isEmpty())
  {
    FShared.clear();
    FSharedNames.clear();
    emit sharedFilesChanged();
  }
}

QString AndroidFiles::DisplayName(const QString & Uri)
{
#ifdef Q_OS_ANDROID
  if (Uri.startsWith(QStringLiteral("content:")))
  {
    QJniEnvironment Env;
    QJniObject Parsed = QJniObject::callStaticObjectMethod("android/net/Uri", "parse",
      "(Ljava/lang/String;)Landroid/net/Uri;", QJniObject::fromString(Uri).object<jstring>());
    QJniObject Resolver = Context().callObjectMethod("getContentResolver", "()Landroid/content/ContentResolver;");
    jobjectArray Columns = Env->NewObjectArray(1, Env->FindClass("java/lang/String"),
      QJniObject::fromString(QStringLiteral("_display_name")).object<jstring>());
    QJniObject Cursor = Resolver.callObjectMethod("query",
      "(Landroid/net/Uri;[Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;)Landroid/database/Cursor;",
      Parsed.object(), Columns, nullptr, nullptr, nullptr);
    QString Name;
    if (!Env.checkAndClearExceptions() && Cursor.isValid())
    {
      if (Cursor.callMethod<jboolean>("moveToFirst"))
      {
        Name = Cursor.callObjectMethod("getString", "(I)Ljava/lang/String;", jint(0)).toString();
      }
      Cursor.callMethod<void>("close");
    }
    Env.checkAndClearExceptions();
    Name.replace(QLatin1Char('/'), QLatin1Char('_'));
    if (!Name.isEmpty())
    {
      return Name;
    }
  }
#endif
  return QFileInfo(QUrl(Uri).isLocalFile() ? QUrl(Uri).toLocalFile() : Uri).fileName();
}

bool AndroidFiles::pinShortcut(const QString & Site)
{
#ifdef Q_OS_ANDROID
  QJniEnvironment Env;
  QJniObject Ctx = Context();
  QJniObject ManagerClass = QJniObject::callStaticObjectMethod("java/lang/Class", "forName",
    "(Ljava/lang/String;)Ljava/lang/Class;", QJniObject::fromString(QStringLiteral("android.content.pm.ShortcutManager")).object<jstring>());
  QJniObject Manager = Ctx.callObjectMethod("getSystemService", "(Ljava/lang/Class;)Ljava/lang/Object;", ManagerClass.object());
  if (Env.checkAndClearExceptions() || !Manager.isValid() || !Manager.callMethod<jboolean>("isRequestPinShortcutSupported"))
  {
    Env.checkAndClearExceptions();
    return false;
  }
  QString Package = Ctx.callObjectMethod("getPackageName", "()Ljava/lang/String;").toString();
  QJniObject Intent("android/content/Intent", "(Ljava/lang/String;)V", QJniObject::fromString(QLatin1String(OpenSiteAction)).object<jstring>());
  Intent.callObjectMethod("setClassName", "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;",
    QJniObject::fromString(Package).object<jstring>(),
    QJniObject::fromString(QStringLiteral("org.qtproject.qt.android.bindings.QtActivity")).object<jstring>());
  Intent.callObjectMethod("putExtra", "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;",
    QJniObject::fromString(QLatin1String(SiteExtra)).object<jstring>(), QJniObject::fromString(Site).object<jstring>());
  QJniObject Info = Ctx.callObjectMethod("getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;");
  jint IconId = Info.getField<jint>("icon");
  QJniObject Icon = QJniObject::callStaticObjectMethod("android/graphics/drawable/Icon", "createWithResource",
    "(Landroid/content/Context;I)Landroid/graphics/drawable/Icon;", Ctx.object(), IconId);
  QString Label = Site.section(QLatin1Char('/'), -1);
  QJniObject Builder("android/content/pm/ShortcutInfo$Builder", "(Landroid/content/Context;Ljava/lang/String;)V",
    Ctx.object(), QJniObject::fromString(QStringLiteral("site:") + Site).object<jstring>());
  Builder.callObjectMethod("setShortLabel", "(Ljava/lang/CharSequence;)Landroid/content/pm/ShortcutInfo$Builder;",
    QJniObject::fromString(Label.left(25)).object<jstring>());
  Builder.callObjectMethod("setLongLabel", "(Ljava/lang/CharSequence;)Landroid/content/pm/ShortcutInfo$Builder;",
    QJniObject::fromString(Site).object<jstring>());
  Builder.callObjectMethod("setIcon", "(Landroid/graphics/drawable/Icon;)Landroid/content/pm/ShortcutInfo$Builder;", Icon.object());
  Builder.callObjectMethod("setIntent", "(Landroid/content/Intent;)Landroid/content/pm/ShortcutInfo$Builder;", Intent.object());
  QJniObject Shortcut = Builder.callObjectMethod("build", "()Landroid/content/pm/ShortcutInfo;");
  bool Ok = Manager.callMethod<jboolean>("requestPinShortcut",
    "(Landroid/content/pm/ShortcutInfo;Landroid/content/IntentSender;)Z", Shortcut.object(), nullptr);
  return !Env.checkAndClearExceptions() && Ok;
#else
  Q_UNUSED(Site);
  return false;
#endif
}
