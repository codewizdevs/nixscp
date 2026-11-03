// Linux desktop integration (see DesktopIntegration.h). Ports of
// windows/Tools.cpp (OpenFolderInExplorer, CreateDesktopSessionShortCut),
// windows/GUITools.cpp (OpenSessionInPutty), windows/UserInterface.cpp
// (FlashOnBackground), forms/CustomScpExplorer.cpp (ApplicationMinimize,
// ApplicationRestore, TrayIconClick, PopupTrayBalloon, OperationComplete,
// taskbar progress) and forms/Login.cpp (DesktopIconActionExecute).
#include "gui/DesktopIntegration.h"
#include "core/Platform.h"
#include "gui/CustomCommandRunner.h"
#include "gui/GuiUserInterface.h"
#include "gui/ProgressDialog.h"
#include "core/Configuration.h"
#include "core/CustomCommands.h"
#include "core/FileSystem.h"
#include "core/IniStorage.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QCursor>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QWindowStateChangeEvent>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMainWindow>
#include <QMenu>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTemporaryFile>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QWindow>

#ifdef NIXSCP_HAVE_QTDBUS
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#endif

#include <memory>
#include <unistd.h>

namespace DesktopIntegration
{

//---------------------------------------------------------------------------
// External programs

static CommandRunner & Runner()
{
  static CommandRunner Instance;
  return Instance;
}

void SetCommandRunner(const CommandRunner & ARunner)
{
  Runner() = ARunner;
}

static bool StartDetached(const QString & Program, const QStringList & Arguments)
{
  if (Runner())
  {
    return Runner()(Program, Arguments);
  }
  return QProcess::startDetached(Program, Arguments);
}

// Runs Program asynchronously; Finished(Ok) on the GUI thread
static void StartAsync(const QString & Program, const QStringList & Arguments, const std::function<void(bool Ok)> & Finished)
{
  if (Runner())
  {
    bool Ok = Runner()(Program, Arguments);
    if (Finished)
    {
      Finished(Ok);
    }
    return;
  }
  QString Path = QStandardPaths::findExecutable(Program);
  if (Path.isEmpty())
  {
    if (Finished)
    {
      Finished(false);
    }
    return;
  }
  auto * Process = new QProcess(qApp);
  Process->setProcessChannelMode(QProcess::MergedChannels);
  QObject::connect(Process, &QProcess::finished, Process, [Process, Finished](int ExitCode, QProcess::ExitStatus Status)
  {
    Process->deleteLater();
    if (Finished)
    {
      Finished((Status == QProcess::NormalExit) && (ExitCode == 0));
    }
  });
  QObject::connect(Process, &QProcess::errorOccurred, Process, [Process, Finished](QProcess::ProcessError Error)
  {
    if (Error == QProcess::FailedToStart)
    {
      Process->deleteLater();
      if (Finished)
      {
        Finished(false);
      }
    }
  });
  Process->start(Path, Arguments);
}

// GVariant text format string literal (gdbus arguments)
static QString GVariantString(const QString & S)
{
  QString Result = S;
  Result.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
  Result.replace(QLatin1Char('\''), QStringLiteral("\\'"));
  Result.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
  return QLatin1Char('\'') + Result + QLatin1Char('\'');
}

//---------------------------------------------------------------------------
// Explore Directory

static void ShowInFileManager(const QString & Method, const QString & Path, const QString & Fallback)
{
  QString Uri = QUrl::fromLocalFile(Path).toString(QUrl::FullyEncoded);
  auto OpenFallback = [Fallback](bool Ok)
  {
    if (!Ok)
    {
      if (Runner())
      {
        Runner()(QStringLiteral("xdg-open"), {Fallback});
      }
      else
      {
        QDesktopServices::openUrl(QUrl::fromLocalFile(Fallback));
      }
    }
  };
#ifdef Q_OS_MACOS
  // Finder: "open -R" reveals (selects) the item, "open" shows the folder
  Q_UNUSED(Uri);
  Q_UNUSED(OpenFallback);
  QStringList Arguments{Path};
  if (Method == QStringLiteral("ShowItems"))
  {
    Arguments.prepend(QStringLiteral("-R"));
  }
  if (Runner())
  {
    Runner()(QStringLiteral("open"), Arguments);
  }
  else
  {
    QProcess::startDetached(QStringLiteral("open"), Arguments);
  }
  return;
#endif
#ifdef NIXSCP_HAVE_QTDBUS
  if (!Runner())
  {
    QDBusConnection Bus = QDBusConnection::sessionBus();
    if (Bus.isConnected())
    {
      QDBusMessage Message = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.FileManager1"),
        QStringLiteral("/org/freedesktop/FileManager1"), QStringLiteral("org.freedesktop.FileManager1"), Method);
      Message << QStringList{Uri} << QString();
      auto * Watcher = new QDBusPendingCallWatcher(Bus.asyncCall(Message, 5000), qApp);
      QObject::connect(Watcher, &QDBusPendingCallWatcher::finished, Watcher, [Watcher, OpenFallback]()
      {
        Watcher->deleteLater();
        OpenFallback(!Watcher->isError());
      });
      return;
    }
  }
#endif
  StartAsync(QStringLiteral("gdbus"),
    {QStringLiteral("call"), QStringLiteral("--session"), QStringLiteral("--timeout"), QStringLiteral("5"),
     QStringLiteral("--dest"), QStringLiteral("org.freedesktop.FileManager1"),
     QStringLiteral("--object-path"), QStringLiteral("/org/freedesktop/FileManager1"),
     QStringLiteral("--method"), QStringLiteral("org.freedesktop.FileManager1.") + Method,
     QStringLiteral("[%1]").arg(GVariantString(Uri)), QStringLiteral("''")},
    OpenFallback);
}

void ExploreDirectory(const QString & Path)
{
  ShowInFileManager(QStringLiteral("ShowFolders"), Path, Path);
}

void ExploreFile(const QString & FileName)
{
  ShowInFileManager(QStringLiteral("ShowItems"), FileName, QFileInfo(FileName).absolutePath());
}

//---------------------------------------------------------------------------
// Places

static void AddPlace(QList<Place> & Result, Place::Kind Kind, const QString & Caption, const QString & Path, const QString & IconName)
{
  if (Path.isEmpty() || !QFileInfo(Path).isDir())
  {
    return;
  }
  QString Clean = QDir::cleanPath(Path);
  for (const Place & Existing : Result)
  {
    if (Existing.Path == Clean)
    {
      return;
    }
  }
  Place P;
  P.Type = Kind;
  P.Caption = Caption;
  P.Path = Clean;
  P.IconName = IconName;
  Result.append(P);
}

QList<Place> BookmarkPlaces(const QString & BookmarksFile)
{
  QList<Place> Result;
  QFile File(BookmarksFile);
  if (!File.open(QIODevice::ReadOnly))
  {
    return Result;
  }
  const QList<QByteArray> Lines = File.readAll().split('\n');
  for (const QByteArray & RawLine : Lines)
  {
    QString Line = QString::fromUtf8(RawLine).trimmed();
    if (Line.isEmpty())
    {
      continue;
    }
    int Space = Line.indexOf(QLatin1Char(' '));
    QString UrlText = (Space >= 0) ? Line.left(Space) : Line;
    QString Label = (Space >= 0) ? Line.mid(Space + 1).trimmed() : QString();
    QUrl Url(UrlText, QUrl::TolerantMode);
    if (!Url.isLocalFile())
    {
      continue;
    }
    QString Path = Url.toLocalFile();
    if (Label.isEmpty())
    {
      Label = QFileInfo(QDir::cleanPath(Path)).fileName();
    }
    AddPlace(Result, Place::pkBookmark, Label, Path, QStringLiteral("folder"));
  }
  return Result;
}

QList<Place> Places()
{
  QList<Place> Result;
  QString Home = QDir::homePath();
  AddPlace(Result, Place::pkStandard, QStringLiteral("Home"), Home, QStringLiteral("user-home"));
  struct StandardPlace { QStandardPaths::StandardLocation Location; const char * Caption; const char * Icon; };
  static const StandardPlace Standard[] =
  {
    {QStandardPaths::DesktopLocation, "Desktop", "user-desktop"},
    {QStandardPaths::DocumentsLocation, "Documents", "folder-documents"},
    {QStandardPaths::DownloadLocation, "Downloads", "folder-download"},
    {QStandardPaths::MusicLocation, "Music", "folder-music"},
    {QStandardPaths::PicturesLocation, "Pictures", "folder-pictures"},
    {QStandardPaths::MoviesLocation, "Videos", "folder-videos"},
  };
  for (const StandardPlace & S : Standard)
  {
    QString Path = QStandardPaths::writableLocation(S.Location);
    // XDG directories not configured fall back to the home folder
    if (QDir::cleanPath(Path) != QDir::cleanPath(Home))
    {
      AddPlace(Result, Place::pkStandard, QString::fromLatin1(S.Caption), Path, QString::fromLatin1(S.Icon));
    }
  }
  AddPlace(Result, Place::pkStandard, QStringLiteral("File System"), QStringLiteral("/"), QStringLiteral("drive-harddisk"));

  QString ConfigHome = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
  QList<Place> Bookmarks = BookmarkPlaces(ConfigHome + QStringLiteral("/gtk-3.0/bookmarks"));
  if (Bookmarks.isEmpty())
  {
    Bookmarks = BookmarkPlaces(QDir::homePath() + QStringLiteral("/.gtk-bookmarks"));
  }
  for (const Place & B : Bookmarks)
  {
    AddPlace(Result, B.Type, B.Caption, B.Path, B.IconName);
  }

  // Mounted volumes a file manager would show (removable media, /mnt, gvfs...)
  QString RuntimeGvfs = QStringLiteral("/run/user/%1/gvfs/").arg(Platform::UserKey());
  const QList<QStorageInfo> Volumes = QStorageInfo::mountedVolumes();
  for (const QStorageInfo & Volume : Volumes)
  {
    if (!Volume.isValid() || !Volume.isReady())
    {
      continue;
    }
    QString Root = Volume.rootPath();
    bool Show =
      Root.startsWith(QStringLiteral("/media/")) || Root.startsWith(QStringLiteral("/run/media/")) ||
      Root.startsWith(QStringLiteral("/mnt/")) || (Root == QStringLiteral("/mnt")) ||
      Root.startsWith(RuntimeGvfs) ||
      (Root.startsWith(Home + QLatin1Char('/')) && !Volume.fileSystemType().startsWith("fuse.portal"));
    if (!Show)
    {
      continue;
    }
    QString Name = Volume.name();
    if (Name.isEmpty())
    {
      Name = QFileInfo(Root).fileName();
    }
    QString Caption = (Name == Root) ? Root : QStringLiteral("%1 (%2)").arg(Name, Root);
    bool Removable = Root.startsWith(QStringLiteral("/media/")) || Root.startsWith(QStringLiteral("/run/media/"));
    AddPlace(Result, Place::pkVolume, Caption, Root,
      Root.startsWith(RuntimeGvfs) ? QStringLiteral("folder-remote") :
      (Removable ? QStringLiteral("drive-removable-media") : QStringLiteral("drive-harddisk")));
  }
  return Result;
}

void FillPlacesMenu(QMenu * Menu, const std::function<void(const QString & Path)> & Selected)
{
  const QList<Place> List = Places();
  Place::Kind LastKind = Place::pkStandard;
  bool First = true;
  for (const Place & P : List)
  {
    if (!First && (P.Type != LastKind))
    {
      Menu->addSeparator();
    }
    First = false;
    LastKind = P.Type;
    QString Caption = P.Caption;
    Caption.replace(QLatin1Char('&'), QStringLiteral("&&"));
    QIcon Icon = QIcon::fromTheme(P.IconName);
    if (Icon.isNull())
    {
      // no icon theme: the style's generic icons
      QStyle::StandardPixmap Pixmap =
        (P.IconName == QStringLiteral("user-home")) ? QStyle::SP_DirHomeIcon :
        ((P.Type == Place::pkVolume) || (P.Path == QStringLiteral("/"))) ? QStyle::SP_DriveHDIcon : QStyle::SP_DirIcon;
      Icon = QApplication::style()->standardIcon(Pixmap);
    }
    QAction * Action = Menu->addAction(Icon, Caption);
    Action->setToolTip(P.Path);
    Action->setData(P.Path);
    QString Path = P.Path;
    QObject::connect(Action, &QAction::triggered, Menu, [Selected, Path]() { if (Selected) Selected(Path); });
  }
}

void ShowPlacesMenu(QWidget * Anchor, const std::function<void(const QString & Path)> & Selected)
{
  QMenu * Menu = new QMenu(Anchor);
  Menu->setAttribute(Qt::WA_DeleteOnClose);
  Menu->setObjectName(QStringLiteral("PlacesMenu"));
  FillPlacesMenu(Menu, Selected);
  QPoint Pos = (Anchor != nullptr) ? Anchor->mapToGlobal(QPoint(0, Anchor->height())) : QCursor::pos();
  Menu->popup(Pos);
}

//---------------------------------------------------------------------------
// Main window: tray icon, progress

class MainWindowIntegration : public QObject
{
public:
  explicit MainWindowIntegration(QMainWindow * Window) : QObject(Window), FWindow(Window)
  {
    Window->installEventFilter(this);
  }

  bool MinimizedToTray() const { return (FTray != nullptr) && FTray->isVisible(); }

  QSystemTrayIcon * Tray() const { return FTray; }

  void Restore()
  {
    // ApplicationRestore / RestoreApp
    if (FWindow == nullptr)
    {
      return;
    }
    if (FTray != nullptr)
    {
      FTray->hide();
    }
    if (FWasMaximized)
    {
      FWindow->showMaximized();
    }
    else
    {
      FWindow->showNormal();
    }
    FWindow->raise();
    FWindow->activateWindow();
  }

  void SetProgressTitle(const QString & Title)
  {
    FProgressTitle = Title;
    UpdateTooltip();
  }

protected:
  bool eventFilter(QObject * Object, QEvent * Event) override
  {
    if ((Object == FWindow) && (Event->type() == QEvent::WindowStateChange))
    {
      if (FWindow->isMinimized())
      {
        FWasMaximized = (static_cast<QWindowStateChangeEvent *>(Event)->oldState() & Qt::WindowMaximized) != 0;
        if (Configuration::Instance().MinimizeToTray && QSystemTrayIcon::isSystemTrayAvailable())
        {
          // ApplicationMinimize
          EnsureTray();
          UpdateTooltip();
          FTray->show();
          QPointer<QMainWindow> Window(FWindow);
          QTimer::singleShot(0, this, [Window]() { if (Window != nullptr) Window->hide(); });
        }
      }
      else if (MinimizedToTray() && !FWindow->isMinimized() && FWindow->isVisible())
      {
        FTray->hide();
      }
    }
    else if ((Object == FWindow) && (Event->type() == QEvent::WindowTitleChange))
    {
      UpdateTooltip();
    }
    return QObject::eventFilter(Object, Event);
  }

private:
  QPointer<QMainWindow> FWindow;
  QSystemTrayIcon * FTray = nullptr;
  bool FWasMaximized = false;
  QString FProgressTitle;

  void EnsureTray()
  {
    if (FTray != nullptr)
    {
      return;
    }
    FTray = new QSystemTrayIcon(this);
    FTray->setObjectName(QStringLiteral("TrayIcon"));
    QIcon Icon = FWindow->windowIcon();
    if (Icon.isNull())
    {
      Icon = QApplication::windowIcon();
    }
    if (Icon.isNull())
    {
      Icon = QIcon::fromTheme(QStringLiteral("nixscp"), QIcon::fromTheme(QStringLiteral("network-server")));
    }
    FTray->setIcon(Icon);
    // StatusNotifierItem hosts often show only the menu: offer Restore there too
    QMenu * Menu = new QMenu(FWindow);
    QAction * RestoreAction = Menu->addAction(QStringLiteral("&Restore"));
    connect(RestoreAction, &QAction::triggered, this, [this]() { Restore(); });
    FTray->setContextMenu(Menu);
    connect(FTray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason Reason)
    {
      // TrayIconClick
      if ((Reason == QSystemTrayIcon::Trigger) || (Reason == QSystemTrayIcon::DoubleClick))
      {
        Restore();
      }
    });
    connect(FTray, &QSystemTrayIcon::messageClicked, this, [this]() { Restore(); });
  }

  void UpdateTooltip()
  {
    // UpdateTrayIcon: the hint is the main window caption (with the progress)
    if ((FTray != nullptr) && (FWindow != nullptr))
    {
      QString Title = FWindow->windowTitle();
      if (!FProgressTitle.isEmpty() && !Title.startsWith(FProgressTitle))
      {
        Title = FProgressTitle + QStringLiteral(" - ") + Title;
      }
      FTray->setToolTip(Title);
    }
  }
};

static QPointer<MainWindowIntegration> & Integration()
{
  static QPointer<MainWindowIntegration> Instance;
  return Instance;
}

void InstallMainWindow(QMainWindow * Window)
{
  if ((Window == nullptr) || Window->property("NixSCPDesktopIntegration").toBool())
  {
    return;
  }
  Window->setProperty("NixSCPDesktopIntegration", true);
  Integration() = new MainWindowIntegration(Window);
}

bool IsMinimizedToTray()
{
  return (Integration() != nullptr) && Integration()->MinimizedToTray();
}

void RestoreFromTray()
{
  if (Integration() != nullptr)
  {
    Integration()->Restore();
  }
}

//---------------------------------------------------------------------------
// Taskbar progress (com.canonical.Unity.LauncherEntry)

static QString DesktopFileId()
{
  QString Name = QGuiApplication::desktopFileName();
  if (Name.isEmpty())
  {
    Name = QStringLiteral("nixscp");
  }
  if (!Name.endsWith(QStringLiteral(".desktop")))
  {
    Name += QStringLiteral(".desktop");
  }
  return Name;
}

static void EmitLauncherEntry(int Percent)
{
  QString AppUri = QStringLiteral("application://") + DesktopFileId();
  QString ObjectPath = QStringLiteral("/com/canonical/unity/launcherentry/%1").arg(qHash(AppUri));
  bool Visible = (Percent >= 0);
  double Progress = Visible ? (Percent / 100.0) : 0.0;
#ifdef NIXSCP_HAVE_QTDBUS
  if (!Runner())
  {
    QDBusConnection Bus = QDBusConnection::sessionBus();
    if (Bus.isConnected())
    {
      QDBusMessage Signal = QDBusMessage::createSignal(ObjectPath, QStringLiteral("com.canonical.Unity.LauncherEntry"),
        QStringLiteral("Update"));
      QVariantMap Properties;
      Properties.insert(QStringLiteral("progress"), Progress);
      Properties.insert(QStringLiteral("progress-visible"), Visible);
      Signal << AppUri << Properties;
      Bus.send(Signal);
      return;
    }
  }
#endif
  StartAsync(QStringLiteral("gdbus"),
    {QStringLiteral("emit"), QStringLiteral("--session"), QStringLiteral("--object-path"), ObjectPath,
     QStringLiteral("--signal"), QStringLiteral("com.canonical.Unity.LauncherEntry.Update"),
     GVariantString(AppUri),
     QStringLiteral("{'progress': <%1>, 'progress-visible': <%2>}").arg(QString::number(Progress, 'f', 2),
       Visible ? QStringLiteral("true") : QStringLiteral("false"))},
    nullptr);
}

struct TaskbarState
{
  int Current = -1;          // last sent
  int Pending = -1;
  bool TimerRunning = false;
  QElapsedTimer LastSent;
};

static TaskbarState & Taskbar()
{
  static TaskbarState State;
  return State;
}

void SetTaskbarProgress(int Percent)
{
  TaskbarState & State = Taskbar();
  Percent = (Percent < 0) ? -1 : std::min(Percent, 100);
  State.Pending = Percent;
  if (State.Pending == State.Current)
  {
    return;
  }
  // at most two updates a second; hiding is sent at once
  bool Due = !State.LastSent.isValid() || (State.LastSent.elapsed() >= 500) || (Percent < 0);
  if (Due)
  {
    State.Current = Percent;
    State.LastSent.restart();
    EmitLauncherEntry(Percent);
  }
  else if (!State.TimerRunning)
  {
    State.TimerRunning = true;
    QTimer::singleShot(500, qApp, []()
    {
      TaskbarState & S = Taskbar();
      S.TimerRunning = false;
      if (S.Pending != S.Current)
      {
        S.Current = S.Pending;
        S.LastSent.restart();
        EmitLauncherEntry(S.Pending);
      }
    });
  }
}

int ProgressTitlePercent(const QString & Title)
{
  static const QRegularExpression Pattern(QStringLiteral("^\\s*(\\d{1,3})\\s*%"));
  QRegularExpressionMatch Match = Pattern.match(Title);
  if (!Match.hasMatch())
  {
    return -1;
  }
  return std::min(Match.captured(1).toInt(), 100);
}

void SetProgressTitle(const QString & Title)
{
  SetTaskbarProgress(Title.isEmpty() ? -1 : std::max(ProgressTitlePercent(Title), 0));
  if (Integration() != nullptr)
  {
    Integration()->SetProgressTitle(Title);
  }
}

//---------------------------------------------------------------------------
// Attention

static int NotificationsTimeout()
{
  // TWinConfiguration::NotificationsTimeout (Configuration\Interface, seconds);
  // not a Preferences option, so read from the file as is
  bool Ok = false;
  int Value = Configuration::Instance().Ini().ReadRaw(QStringLiteral("Configuration\\Interface"),
    QStringLiteral("NotificationsTimeout"), QStringLiteral("10")).toInt(&Ok);
  return (Ok && (Value > 0)) ? Value : 10;
}

static QWidget * MainWindow(QWidget * Window)
{
  if (Window != nullptr)
  {
    return Window->window();
  }
  for (QWidget * Widget : QApplication::topLevelWidgets())
  {
    if (qobject_cast<QMainWindow *>(Widget) != nullptr)
    {
      return Widget;
    }
  }
  return QApplication::activeWindow();
}

void FlashOnBackground(QWidget * Window)
{
  // FlashOnBackground: only when the application is not in the foreground
  if (!Configuration::Instance().FlashTaskbar || (QApplication::applicationState() == Qt::ApplicationActive))
  {
    return;
  }
  QWidget * Target = MainWindow(Window);
  if (Target != nullptr)
  {
    QApplication::alert(Target);
  }
}

static QString StripMarkup(const QString & Message, QString * MainInstruction)
{
  // ExtractMainInstructions / UnformatMessage
  static const QRegularExpression Pattern(QStringLiteral("^\\*\\*(.*?)\\*\\*"), QRegularExpression::DotMatchesEverythingOption);
  QRegularExpressionMatch Match = Pattern.match(Message);
  if (Match.hasMatch())
  {
    if (MainInstruction != nullptr)
    {
      *MainInstruction = Match.captured(1).trimmed();
    }
    return Message.mid(Match.capturedLength()).trimmed();
  }
  QString Result = Message;
  Result.remove(QStringLiteral("**"));
  return Result.trimmed();
}

bool Notify(const QString & ATitle, const QString & AMessage, NotificationType Type, int TimeoutSeconds)
{
  if (!Configuration::Instance().BalloonNotifications)
  {
    FlashOnBackground();
    return false;
  }
  QString MainInstruction;
  QString Message = StripMarkup(AMessage, &MainInstruction);
  QString Title = ATitle;
  if (!MainInstruction.isEmpty() && (Type == ntInformation))
  {
    // PopupTrayBalloon: the main instruction becomes the title
    Title = MainInstruction;
  }
  else if (!MainInstruction.isEmpty())
  {
    Message = Message.isEmpty() ? MainInstruction : (MainInstruction + QLatin1Char('\n') + Message);
  }
  if (Title.isEmpty())
  {
    // _SMsgDlgWarning / _SMsgDlgError / _SMsgDlgInformation
    Title = (Type == ntError) ? QStringLiteral("Error") : ((Type == ntWarning) ? QStringLiteral("Warning") : QStringLiteral("Information"));
  }
  int Seconds = (TimeoutSeconds > 0) ? TimeoutSeconds : NotificationsTimeout();

  if (!Runner() && IsMinimizedToTray() && (Integration()->Tray() != nullptr) && QSystemTrayIcon::supportsMessages())
  {
    QSystemTrayIcon::MessageIcon Icon = (Type == ntError) ? QSystemTrayIcon::Critical :
      ((Type == ntWarning) ? QSystemTrayIcon::Warning : QSystemTrayIcon::Information);
    Integration()->Tray()->showMessage(Title, Message, Icon, Seconds * 1000);
    return true;
  }

  QString IconName = (Type == ntError) ? QStringLiteral("dialog-error") :
    ((Type == ntWarning) ? QStringLiteral("dialog-warning") : QStringLiteral("nixscp"));
#ifdef NIXSCP_HAVE_QTDBUS
  if (!Runner())
  {
    QDBusConnection Bus = QDBusConnection::sessionBus();
    if (Bus.isConnected())
    {
      QDBusMessage Call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.Notifications"),
        QStringLiteral("/org/freedesktop/Notifications"), QStringLiteral("org.freedesktop.Notifications"),
        QStringLiteral("Notify"));
      QVariantMap Hints;
      Hints.insert(QStringLiteral("desktop-entry"), DesktopFileId().chopped(8));
      Call << QStringLiteral("NixSCP") << 0u << IconName << Title << Message << QStringList() << Hints << (Seconds * 1000);
      Bus.asyncCall(Call);
      return true;
    }
  }
#endif
  QString DesktopEntryName = DesktopFileId().chopped(8);
  StartAsync(QStringLiteral("gdbus"),
    {QStringLiteral("call"), QStringLiteral("--session"), QStringLiteral("--dest"), QStringLiteral("org.freedesktop.Notifications"),
     QStringLiteral("--object-path"), QStringLiteral("/org/freedesktop/Notifications"),
     QStringLiteral("--method"), QStringLiteral("org.freedesktop.Notifications.Notify"),
     QStringLiteral("'NixSCP'"), QStringLiteral("0"), GVariantString(IconName), GVariantString(Title), GVariantString(Message),
     QStringLiteral("[]"), QStringLiteral("{'desktop-entry': <%1>}").arg(GVariantString(DesktopEntryName)),
     QString::number(Seconds * 1000)},
    [Title, Message, Seconds, IconName](bool Ok)
    {
      if (!Ok && !QStandardPaths::findExecutable(QStringLiteral("notify-send")).isEmpty())
      {
        StartDetached(QStringLiteral("notify-send"),
          {QStringLiteral("--app-name=NixSCP"), QStringLiteral("--icon=") + IconName,
           QStringLiteral("--expire-time=%1").arg(Seconds * 1000), Title, Message});
      }
    });
  return true;
}

void OperationComplete(QWidget * Window, qint64 ElapsedMs, const QString & Message)
{
  Configuration & Config = Configuration::Instance();
  if (Config.BeepOnFinish && (ElapsedMs > static_cast<qint64>(Config.BeepOnFinishAfter) * 1000))
  {
    // BeepSound (a Windows sound alias/file): the desktop bell
    QApplication::beep();
  }
  QWidget * Target = MainWindow(Window);
  bool Background = (QApplication::applicationState() != Qt::ApplicationActive) ||
    ((Target != nullptr) && (Target->isMinimized() || !Target->isVisible()));
  if (Background)
  {
    // BALLOON_OPERATION_COMPLETE
    if (!Notify(QString(), Message.isEmpty() ? QStringLiteral("**Operation was completed**") : Message, ntInformation))
    {
      return;   // Notify flashed instead
    }
    FlashOnBackground(Target);
  }
}

//---------------------------------------------------------------------------
// Terminal client (OpenSessionInPutty)

static QString ShellSingleQuote(const QString & S)
{
  QString Result = S;
  Result.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
  return QLatin1Char('\'') + Result + QLatin1Char('\'');
}

// %host/%port... of WinSCP proxy commands to OpenSSH ProxyCommand tokens
static QString ProxyCommandForSsh(const SessionData & Data)
{
  if (Data.ProxyMethod == pmCmd)
  {
    QString Command = Data.ProxyLocalCommand;
    if (Command.isEmpty())
    {
      return QString();
    }
    // OpenSSH tokens: escape literal %, then map WinSCP's patterns
    QString Result;
    for (int I = 0; I < Command.size(); I++)
    {
      if (Command[I] == QLatin1Char('%'))
      {
        QString Rest = Command.mid(I + 1);
        struct Map { const char * Name; QString Value; };
        const Map Maps[] =
        {
          {"proxyhost", Data.ProxyHost}, {"proxyport", QString::number(Data.ProxyPort)},
          {"host", QStringLiteral("%h")}, {"port", QStringLiteral("%p")},
          {"user", Data.ProxyUsername}, {"pass", Data.ProxyPassword},
        };
        bool Found = false;
        for (const Map & M : Maps)
        {
          QString Name = QString::fromLatin1(M.Name);
          if (Rest.startsWith(Name, Qt::CaseInsensitive))
          {
            QString Value = M.Value;
            if (!Value.startsWith(QLatin1Char('%')))
            {
              Value.replace(QLatin1Char('%'), QStringLiteral("%%"));
            }
            Result += Value;
            I += Name.size();
            Found = true;
            break;
          }
        }
        if (!Found)
        {
          if (Rest.startsWith(QLatin1Char('%')))
          {
            I++;
          }
          Result += QStringLiteral("%%");
        }
      }
      else
      {
        Result += Command[I];
      }
    }
    return Result;
  }
  if ((Data.ProxyMethod == pmSocks4) || (Data.ProxyMethod == pmSocks5) || (Data.ProxyMethod == pmHTTP))
  {
    // OpenBSD netcat (the "nc" of Debian/Ubuntu/Fedora's nmap-ncat lacks -X)
    if (QStandardPaths::findExecutable(QStringLiteral("nc")).isEmpty() || Data.ProxyHost.isEmpty())
    {
      return QString();
    }
    QString Version = (Data.ProxyMethod == pmSocks4) ? QStringLiteral("4") :
      ((Data.ProxyMethod == pmSocks5) ? QStringLiteral("5") : QStringLiteral("connect"));
    QString Result = QStringLiteral("nc -X %1 -x %2").arg(Version,
      ShellSingleQuote(Data.ProxyHost + QLatin1Char(':') + QString::number(Data.ProxyPort)));
    if ((Data.ProxyMethod == pmHTTP) && !Data.ProxyUsername.isEmpty())
    {
      Result += QStringLiteral(" -P ") + ShellSingleQuote(Data.ProxyUsername);
    }
    return Result + QStringLiteral(" %h %p");
  }
  return QString();
}

static bool IsSshProgram(const QString & Arg)
{
  QString Name = QFileInfo(Arg).fileName();
  return (Name == QStringLiteral("ssh")) || (Name == QStringLiteral("ssh.exe"));
}

static bool IsTelnetProgram(const QString & Arg)
{
  return (QFileInfo(Arg).fileName() == QStringLiteral("telnet"));
}

namespace
{
struct TerminalEmulator
{
  const char * Name;
  const char * ExecSwitch;    // "execute the rest of the command line" (replaces -e)
  const char * Desktops;      // XDG_CURRENT_DESKTOP values it is the default terminal of
};

// Checked against the programs' --help / manual pages: gnome-terminal and
// ptyxis (GNOME 47+, Ubuntu 25.10+) take "--" (ptyxis also a hidden -e),
// kgx (GNOME Console) "--" or -e, konsole -e, xfce4-terminal/mate-terminal/
// terminator -x (their -e takes one string), xterm/urxvt/alacritty -e, foot
// and kitty the bare command; xdg-terminal-exec (freedesktop default
// terminal proposal) takes the bare command and starts the user's terminal.
const TerminalEmulator TerminalEmulators[] =
{
  {"xdg-terminal-exec", "", ""},
  {"ptyxis", "--", "GNOME"}, {"kgx", "--", "GNOME"}, {"gnome-terminal", "--", "GNOME:Unity:X-Cinnamon:Budgie"},
  {"konsole", "-e", "KDE"}, {"xfce4-terminal", "-x", "XFCE"}, {"mate-terminal", "-x", "MATE"},
  {"lxterminal", "-e", "LXDE"}, {"qterminal", "-e", "LXQt"}, {"io.elementary.terminal", "-x", "Pantheon"},
  {"terminator", "-x", ""}, {"alacritty", "-e", ""}, {"foot", "", ""}, {"kitty", "", ""}, {"wezterm", "start --", ""},
  {"xterm", "-e", ""}, {"urxvt", "-e", ""},
};

bool IsCurrentDesktop(const char * Desktops)
{
  const QStringList Current = qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(QLatin1Char(':'), Qt::SkipEmptyParts);
  const QStringList Wanted = QString::fromLatin1(Desktops).split(QLatin1Char(':'), Qt::SkipEmptyParts);
  for (const QString & Desktop : Current)
  {
    if (Wanted.contains(Desktop, Qt::CaseInsensitive))
    {
      return true;
    }
  }
  return false;
}

QString ApplyTerminalEmulator(const TerminalEmulator & Emulator, QStringList & Arguments)
{
  if (!Arguments.isEmpty() && (Arguments.first() == QStringLiteral("-e")))
  {
    Arguments.removeFirst();
    const QStringList Switch = QString::fromLatin1(Emulator.ExecSwitch).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (int I = static_cast<int>(Switch.size()) - 1; I >= 0; I--)
    {
      Arguments.prepend(Switch[I]);
    }
  }
  return QString::fromLatin1(Emulator.Name);
}
} // namespace

QString ResolveTerminalProgram(const QString & Program, QStringList & Arguments)
{
#ifdef Q_OS_MACOS
  if (Program == QStringLiteral("x-terminal-emulator"))
  {
    // Terminal.app opens a private .command script that removes itself,
    // clears the window and runs the command line (no typed-in command, no
    // permission to control Terminal needed)
    if (!Arguments.isEmpty() && (Arguments.first() == QStringLiteral("-e")))
    {
      Arguments.removeFirst();
    }
    QString Line;
    for (const QString & Arg : std::as_const(Arguments))
    {
      QString Quoted = Arg;
      Quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
      Line += QStringLiteral(" '") + Quoted + QLatin1Char('\'');
    }
    QTemporaryFile Script(QDir::tempPath() + QStringLiteral("/nixscp-terminal-XXXXXX.command"));
    Script.setAutoRemove(false);
    if (Script.open())
    {
      Script.write(("#!/bin/sh\nrm -f \"$0\"\nclear\nexec" + Line + "\n").toUtf8());
      Script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
      Script.close();
      Arguments = QStringList{QStringLiteral("-a"), QStringLiteral("Terminal"), Script.fileName()};
      return QStringLiteral("open");
    }
    return Program;
  }
#endif
  if ((Program != QStringLiteral("x-terminal-emulator")) || !QStandardPaths::findExecutable(Program).isEmpty())
  {
    return Program;
  }
  // Debian's alternative is missing (Fedora, Arch, openSUSE...): the user's
  // ($TERMINAL, xdg-terminal-exec), the desktop's, else any known terminal
  // emulator, with its switch for "execute the rest of the command line"
  QString User = qEnvironmentVariable("TERMINAL");
  if (!User.isEmpty() && !QStandardPaths::findExecutable(User).isEmpty())
  {
    for (const TerminalEmulator & E : TerminalEmulators)
    {
      if (QFileInfo(User).fileName() == QString::fromLatin1(E.Name))
      {
        ApplyTerminalEmulator(E, Arguments);
        return User;
      }
    }
    return User;   // unknown emulators are assumed to take the common -e
  }
  for (bool DesktopOnly : {true, false})
  {
    for (const TerminalEmulator & E : TerminalEmulators)
    {
      bool Candidate = DesktopOnly ?
        ((qstrcmp(E.Name, "xdg-terminal-exec") == 0) || IsCurrentDesktop(E.Desktops)) : true;
      if (Candidate && !QStandardPaths::findExecutable(QString::fromLatin1(E.Name)).isEmpty())
      {
        return ApplyTerminalEmulator(E, Arguments);
      }
    }
  }
  return Program;
}

bool BuildTerminalCommand(const SessionData & AData, const QString & APassword, const QString & RemoteDirectory,
  bool ChangeDirectory, TerminalCommand & Result, QString & Error,
  const std::function<bool(const QStringList & Prompts, const QStringList & Defaults, QStringList & Values)> & Prompts,
  const QString & CommandTemplate)
{
  Configuration & Config = Configuration::Instance();
  Result = TerminalCommand();
  Error.clear();
  // putty does not support resolving environment variables in session settings
  SessionData Data = AData;
  Data.ExpandEnvironmentVariables();
  // Values starting with "-" would be taken as ssh options
  // (e.g. "-oProxyCommand=...") and run arbitrary commands
  if (Data.HostNameExpanded().startsWith(QLatin1Char('-')) || Data.UserNameExpanded().startsWith(QLatin1Char('-')) ||
      Data.TunnelHostName.startsWith(QLatin1Char('-')) || Data.TunnelUserName.startsWith(QLatin1Char('-')))
  {
    Error = QStringLiteral("Invalid host or user name.");
    return false;
  }
  bool Ssh = Data.IsSsh();
  bool Telnet = !Ssh && (Data.FSProtocol == fsFTP) && Config.TelnetForFtpInPutty;
  if (!Ssh)
  {
    // the terminal client is an SSH client: !# is its port, not the FTP/WebDAV one
    Data.PortNumber = 22;
  }

  QString Template = CommandTemplate;
  if (Template.isEmpty())
  {
    Template = Config.PuttyPath.trimmed().isEmpty() ? Configuration::DefaultPuttyPath() : Config.PuttyPath;
  }
  Template = SessionData::ExpandEnvironmentVariables(Template);

  QString Password;
  if (Config.PuttyPassword)
  {
    Password = APassword.isEmpty() ? Data.Password : APassword;
  }
  SessionData PatternData = Data;
  PatternData.Password = Password;
  CustomCommandData CommandData = CustomCommandData::FromSession(PatternData, Password);
  QString RemotePath = RemoteDirectory.isEmpty() ? Data.RemoteDirectory : RemoteDirectory;
  LocalCustomCommand Local(CommandData, RemotePath, Data.LocalDirectoryExpanded());
  Local.SetShellQuoting(false);  // arguments are expanded one by one, no shell
  InteractiveCustomCommand Interactive(&Local,
    Prompts ? InteractiveCustomCommand::PromptsCallback(Prompts) : InteractiveCustomCommand::PromptsCallback());

  QStringList Args;
  try
  {
    // Prompts and local command output on the whole command line (one
    // prompts dialog), session patterns per argument after splitting, so
    // that values (passwords, paths) never need quoting
    QString Command = Interactive.Complete(Template, false);
    const QStringList Split = QProcess::splitCommand(Command);
    for (const QString & Arg : Split)
    {
      QString Expanded = Local.Complete(Arg, true);
      if (Data.UserName.isEmpty() && Expanded.startsWith(QLatin1Char('@')) && (Expanded.size() > 1))
      {
        Expanded.remove(0, 1);  // "!U@!@" without user name
      }
      Args.append(Expanded);
    }
  }
  catch (const AbortException &)
  {
    Error.clear();
    return false;
  }
  catch (const ExtException & E)
  {
    Error = E.Message();
    return false;
  }
  if (Args.isEmpty())
  {
    Error = QStringLiteral("No terminal client command is configured.");
    return false;
  }
  bool SiteCommand = Local.IsSiteCommand(Template);
  bool PasswordCommand = Local.IsPasswordCommand(Template);

  int ClientIndex = -1;
  for (int I = 0; I < Args.size(); I++)
  {
    if (IsSshProgram(Args[I]) || IsTelnetProgram(Args[I]))
    {
      ClientIndex = I;
      break;
    }
  }

  if (Telnet)
  {
    // TelnetForFtpInPutty: telnet to the host (PuTTY -telnet; no password)
    if (ClientIndex >= 0)
    {
      Args = Args.mid(0, ClientIndex);
    }
    Args << QStringLiteral("telnet") << Data.HostNameExpanded();
    Password.clear();
  }
  else
  {
    if (ClientIndex < 0)
    {
      // No client in the command (e.g. "konsole -e"): run OpenSSH
      ClientIndex = static_cast<int>(Args.size());
      Args << QStringLiteral("ssh");
      SiteCommand = false;
    }
    if (IsTelnetProgram(Args[ClientIndex]))
    {
      ClientIndex = -1;
    }
  }

  if (!Telnet && (ClientIndex >= 0))
  {
    // OpenSSH equivalents of the PuTTY session settings (ExportSessionToPutty /
    // the command line of the UWP variant)
    QStringList Options;
    QString KeyFile = Data.PublicKeyFile;
    if (!KeyFile.isEmpty())
    {
      if (!KeyFile.endsWith(QStringLiteral(".ppk"), Qt::CaseInsensitive))
      {
        Options << QStringLiteral("-i") << KeyFile;
      }
      else
      {
        // OpenSSH cannot read PuTTY keys: use an OpenSSH key next to it, if any
        QString Base = KeyFile.left(KeyFile.size() - 4);
        if (QFileInfo::exists(Base))
        {
          Options << QStringLiteral("-i") << Base;
        }
      }
    }
    if (!Data.TryAgent)
    {
      Options << QStringLiteral("-o") << QStringLiteral("IdentityAgent=none");
    }
    else if (Data.AgentFwd)
    {
      Options << QStringLiteral("-A");
    }
    if (Data.Compression)
    {
      Options << QStringLiteral("-C");
    }
    if (Data.AddressFamily == afIPv4)
    {
      Options << QStringLiteral("-4");
    }
    else if (Data.AddressFamily == afIPv6)
    {
      Options << QStringLiteral("-6");
    }
    if (Data.Tunnel && !Data.TunnelHostName.isEmpty())
    {
      QString Jump = Data.TunnelHostName;
      if (SessionData::IsIPv6Literal(Jump))
      {
        Jump = QLatin1Char('[') + Jump + QLatin1Char(']');
      }
      if (!Data.TunnelUserName.isEmpty())
      {
        Jump = Data.TunnelUserName + QLatin1Char('@') + Jump;
      }
      if ((Data.TunnelPortNumber > 0) && (Data.TunnelPortNumber != 22))
      {
        Jump += QStringLiteral(":%1").arg(Data.TunnelPortNumber);
      }
      Options << QStringLiteral("-J") << Jump;
    }
    else
    {
      QString Proxy = ProxyCommandForSsh(Data);
      if (!Proxy.isEmpty())
      {
        Options << QStringLiteral("-o") << (QStringLiteral("ProxyCommand=") + Proxy);
      }
    }
    if ((Data.PingType != ptOff) && (Data.PingInterval > 0))
    {
      Options << QStringLiteral("-o") << QStringLiteral("ServerAliveInterval=%1").arg(Data.PingInterval);
    }
    if (!Data.LogicalHostName.isEmpty())
    {
      Options << QStringLiteral("-o") << (QStringLiteral("HostKeyAlias=") + Data.LogicalHostName);
    }
    if (!SiteCommand)
    {
      // The command does not say where to connect (PuTTY would load the session)
      if (Data.PortNumber != 22)
      {
        Options << QStringLiteral("-p") << QString::number(Data.PortNumber);
      }
      if (!Data.UserNameExpanded().isEmpty())
      {
        Options << QStringLiteral("-l") << Data.UserNameExpanded();
      }
    }
    for (int I = 0; I < Options.size(); I++)
    {
      Args.insert(ClientIndex + 1 + I, Options[I]);
    }
    if (!SiteCommand)
    {
      Args << Data.HostNameExpanded();
    }
    if (ChangeDirectory && !RemotePath.isEmpty() && (Args.last() != QStringLiteral("-t")))
    {
      // NixSCP: the shell starts in the remote panel's directory
      Args << QStringLiteral("-t") << QStringLiteral("cd %1 && exec \"$SHELL\" -l").arg(ShellSingleQuote(RemotePath));
    }
    // "Remember session password and pass it to PuTTY": OpenSSH has no -pw, so
    // NixSCP itself acts as SSH_ASKPASS, reading a one-time private file (src/main.cpp)
    if (!Password.isEmpty() && !PasswordCommand)
    {
      QString RuntimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
      if (RuntimeDir.isEmpty())
      {
        RuntimeDir = QDir::tempPath();
      }
      QTemporaryFile File(RuntimeDir + QStringLiteral("/nixscp-askpass-XXXXXX"));
      File.setAutoRemove(false);
      if (File.open())
      {
        File.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        File.write(Password.toUtf8());
        File.close();
        Result.AskPassFile = File.fileName();
        QStringList Env{QStringLiteral("env"), QStringLiteral("SSH_ASKPASS=%1").arg(QCoreApplication::applicationFilePath()),
          QStringLiteral("SSH_ASKPASS_REQUIRE=force"), QStringLiteral("NIXSCP_ASKPASS_FILE=%1").arg(Result.AskPassFile),
          // only the target host's password prompt gets the password (not a jump host's)
          QStringLiteral("NIXSCP_ASKPASS_HOST=%1").arg(Data.HostNameExpanded())};
        for (int I = 0; I < Env.size(); I++)
        {
          Args.insert(ClientIndex + I, Env[I]);
        }
      }
    }
  }

  Result.Program = Args.takeFirst();
  Result.Arguments = Args;
  return true;
}

bool OpenSessionInTerminal(QWidget * Parent, const SessionData & Data, const QString & Password,
  const QString & RemoteDirectory, bool ChangeDirectory)
{
  TerminalCommand Command;
  QString Error;
  bool Built = BuildTerminalCommand(Data, Password, RemoteDirectory, ChangeDirectory, Command, Error,
    [Parent](const QStringList & Prompts, const QStringList & Defaults, QStringList & Values)
    {
      // TWinInteractiveCustomCommand(..., L"PuTTY", ...)
      return CustomCommandRunner::PromptValues(Parent, QStringLiteral("PuTTY"), Prompts, Defaults, Values);
    });
  if (!Built)
  {
    if (!Error.isEmpty())
    {
      ShowErrorMessage(Parent, Error);
    }
    return false;
  }
  QString Program = ResolveTerminalProgram(Command.Program, Command.Arguments);
  if (!StartDetached(Program, Command.Arguments))
  {
    if (!Command.AskPassFile.isEmpty())
    {
      QFile::remove(Command.AskPassFile);
    }
    // EXECUTE_APP_ERROR
    ShowErrorMessage(Parent, QStringLiteral("Cannot execute \"%1\".").arg(Program),
      QStringLiteral("Check the PuTTY/Terminal client path in Preferences > Integration > Applications."));
    return false;
  }
  if (!Command.AskPassFile.isEmpty())
  {
    // Do not leave the password behind if ssh never asks
    QString FileName = Command.AskPassFile;
    QTimer::singleShot(120000, qApp, [FileName]() { QFile::remove(FileName); });
  }
  return true;
}

//---------------------------------------------------------------------------
// Desktop icon

// Desktop Entry Specification: quoting of an Exec argument
static QString ExecQuote(const QString & Arg)
{
  QString Escaped;
  bool NeedsQuotes = Arg.isEmpty();
  for (QChar C : Arg)
  {
    if ((C == QLatin1Char('"')) || (C == QLatin1Char('`')) || (C == QLatin1Char('$')) || (C == QLatin1Char('\\')))
    {
      Escaped += QLatin1Char('\\');
      NeedsQuotes = true;
    }
    else if (C.isSpace() || QStringLiteral("'><~|&;*?#()").contains(C))
    {
      NeedsQuotes = true;
    }
    Escaped += C;
  }
  Escaped.replace(QLatin1Char('%'), QStringLiteral("%%"));
  return NeedsQuotes ? (QLatin1Char('"') + Escaped + QLatin1Char('"')) : Escaped;
}

// Desktop Entry Specification: escapes of string values
static QString EntryValue(const QString & Value)
{
  QString Result = Value;
  Result.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
  Result.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
  Result.replace(QLatin1Char('\t'), QStringLiteral("\\t"));
  Result.replace(QLatin1Char('\r'), QStringLiteral("\\r"));
  return Result;
}

QString DesktopEntry(const QString & SessionName, ShortCutKind Kind, const QString & InfoTip, const QString & Executable)
{
  QString Program = Executable.isEmpty() ? QCoreApplication::applicationFilePath() : Executable;
  if (Program.isEmpty())
  {
    Program = QStringLiteral("nixscp");
  }
  QString Exec = ExecQuote(Program) + QLatin1Char(' ') + ExecQuote(SessionName);
  QString Name;
  QString Comment = InfoTip;
  if ((Kind == sscFolder) || (Kind == sscWorkspace))
  {
    // WORKSPACE_SWITCH
    Exec += QStringLiteral(" /workspace");
    Name = SessionName.mid(SessionName.lastIndexOf(QLatin1Char('/')) + 1);
    if (Comment.isEmpty())
    {
      // SHORTCUT_INFO_TIP_FOLDER / SHORTCUT_INFO_TIP_WORKSPACE
      Comment = ((Kind == sscFolder) ? QStringLiteral("Opens site folder '%1'") : QStringLiteral("Opens workspace '%1'")).arg(SessionName);
    }
  }
  else
  {
    Name = SessionName.mid(SessionName.lastIndexOf(QLatin1Char('/')) + 1);
    if (Comment.isEmpty())
    {
      // SHORTCUT_INFO_TIP (without the site details)
      Comment = QStringLiteral("Opens site '%1'").arg(SessionName);
    }
  }
  QString Icon = (Kind == sscSite) ? QStringLiteral("nixscp") : QStringLiteral("nixscp");
  QStringList Lines;
  Lines << QStringLiteral("[Desktop Entry]")
        << QStringLiteral("Type=Application")
        << QStringLiteral("Version=1.0")
        << (QStringLiteral("Name=") + EntryValue(Name))
        << (QStringLiteral("Comment=") + EntryValue(Comment))
        << (QStringLiteral("Exec=") + Exec)
        << (QStringLiteral("Icon=") + Icon)
        << QStringLiteral("Terminal=false")
        << QStringLiteral("StartupWMClass=nixscp")
        << QStringLiteral("Categories=Network;FileTransfer;");
  return Lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

// ValidLocalFileName: no slashes (or NUL) in the launcher's file name
static QString ValidDesktopFileName(const QString & Name)
{
  QString Result = Name;
  Result.replace(QLatin1Char('/'), QLatin1Char('_'));
  Result.remove(QChar(0));
  if (Result.isEmpty() || (Result == QStringLiteral(".")) || (Result == QStringLiteral("..")))
  {
    Result = QStringLiteral("NixSCP");
  }
  return Result;
}

QString CreateDesktopIcon(QWidget * Parent, const QString & SessionName, ShortCutKind Kind, const QString & InfoTip,
  bool Confirm, const QString & Directory)
{
  if (Confirm)
  {
    // CONFIRM_CREATE_SHORTCUT / _FOLDER / _WORKSPACE
    QString Message = (Kind == sscSite) ? QStringLiteral("Create desktop shortcut for site '%1'?") :
      ((Kind == sscFolder) ? QStringLiteral("Create desktop shortcut for site folder '%1'?") :
       QStringLiteral("Create desktop shortcut for workspace '%1'?"));
    if (MessageDialog(Parent, QStringLiteral("**%1**").arg(Message.arg(SessionName)), qtConfirmation, qaYes | qaNo,
          QStringLiteral("integration")) != qaYes)
    {
      return QString();
    }
  }
  QString Folder = Directory.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::DesktopLocation) : Directory;
  if (Folder.isEmpty() || !QDir().mkpath(Folder))
  {
    ShowErrorMessage(Parent, QStringLiteral("Cannot create shortcut on the desktop."));
    return QString();
  }
  QString Name = SessionName.mid(SessionName.lastIndexOf(QLatin1Char('/')) + 1);
  QString FileName = QDir(Folder).filePath(ValidDesktopFileName(Name) + QStringLiteral(".desktop"));
  QSaveFile File(FileName);
  if (!File.open(QIODevice::WriteOnly) ||
      (File.write(DesktopEntry(SessionName, Kind, InfoTip).toUtf8()) < 0) ||
      !File.commit())
  {
    ShowErrorMessage(Parent, QStringLiteral("Cannot create shortcut '%1'.").arg(FileName), File.errorString());
    return QString();
  }
  // Launchers on the desktop must be executable (and trusted on GNOME) to run
  QFile::setPermissions(FileName, QFile::permissions(FileName) | QFileDevice::ExeOwner | QFileDevice::ExeUser);
  if (!QStandardPaths::findExecutable(QStringLiteral("gio")).isEmpty() || Runner())
  {
    StartAsync(QStringLiteral("gio"),
      {QStringLiteral("set"), FileName, QStringLiteral("metadata::trusted"), QStringLiteral("true")}, nullptr);
  }
  return FileName;
}

//---------------------------------------------------------------------------
// Local file operations with progress

namespace
{
struct LocalJob
{
  FileOperationProgress Progress;
  LocalOperationResult Result;
};

void RunLocalJob(QWidget * Parent, const QString & TargetDirectory, std::function<LocalOperationResult(FileOperationProgress &)> Work,
  const std::function<void(const LocalOperationResult &)> & Done)
{
  auto Job = std::make_shared<LocalJob>();
  QPointer<ProgressDialog> Progress = new ProgressDialog(Job->Progress, Parent);
  Progress->SetAllowMoveToQueue(false);
  if (!TargetDirectory.isEmpty())
  {
    Progress->SetTarget(TargetDirectory);
  }
  Progress->Start();
  QPointer<QWidget> ParentGuard(Parent);
  QThread * Thread = QThread::create([Job, Work]()
  {
    Job->Result = Work(Job->Progress);
  });
  QObject::connect(Thread, &QThread::finished, qApp, [Thread, Job, Progress, ParentGuard, Done]()
  {
    Thread->deleteLater();
    if (Progress != nullptr)
    {
      Progress->Finish(Job->Result.Ok);
      Progress->deleteLater();
    }
    if (!Job->Result.Ok && !Job->Result.Aborted && !Job->Result.Message.isEmpty())
    {
      ShowErrorMessage(ParentGuard, Job->Result.Message, Job->Result.MoreMessages);
    }
    if (Done)
    {
      Done(Job->Result);
    }
  });
  Thread->start();
}
} // namespace

void RunLocalFileOperation(QWidget * Parent, UserInterface * UI, const LocalFileOperations::Params & Params,
  const std::function<void(const LocalOperationResult &)> & Done)
{
  RunLocalJob(Parent, Params.TargetDirectory, [Params, UI](FileOperationProgress & Progress)
  {
    return LocalFileOperations::Execute(Params, &Progress, UI);
  }, Done);
}

void RunLocalProperties(QWidget * Parent, UserInterface * UI, const LocalFileOperations::PropertiesParams & Params,
  const std::function<void(const LocalOperationResult &)> & Done)
{
  RunLocalJob(Parent, QString(), [Params, UI](FileOperationProgress & Progress)
  {
    return LocalFileOperations::ChangeProperties(Params, &Progress, UI);
  }, Done);
}

} // namespace DesktopIntegration
