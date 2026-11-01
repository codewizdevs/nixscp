// Desktop compatibility verification (GNOME, KDE, Xfce; Wayland and X11):
// - every Icons name resolves to a theme icon on Breeze, Adwaita, Yaru and
//   elementary-xfce (each checked in a child process, as icons are cached;
//   themes not installed are skipped; NIXSCP_VERIFY_ICON_PATHS adds theme
//   roots, e.g. themes unpacked from .deb packages), contact sheets with
//   NIXSCP_VERIFY_CONTACT_SHEETS=<dir>; the icon theme is detected from
//   xfconf (Xfce) and kdeglobals (KDE) without a platform theme
// - Open in PuTTY: terminal emulator detection and argument conventions run
//   end to end with stand-in terminals (gnome-terminal "--", ptyxis "--",
//   kgx "--", konsole "-e", xfce4-terminal "-x", xterm "-e",
//   xdg-terminal-exec) that run a stand-in ssh; also ssh:// URLs
// - Trash: LocalFileOperations::MoveToTrash (home trash, volume without a trash)
// - single instance: listen race between processes, command line + activation token
// - tray icon not available (offscreen): minimizing does not hide the window
// - notifications and Explore Directory go through gdbus (stand-in) and
//   xdg-open (only with a live display: offscreen Qt has no URL services)
// - systemd-inhibit sleep lock (SleepInhibitor), when logind is reachable
// - data/nixscp.desktop, generated launchers and the URL handler entry pass
//   desktop-file-validate (when installed)
//
// GUI test: built with NIXSCP_GUI_TESTS and the GUI objects, runs offscreen.
#ifndef NIXSCP_GUI_TESTS

#include <cstdio>

int main()
{
  std::printf("SKIP: test_verify_desktop needs NIXSCP_GUI_TESTS (Qt Widgets + the GUI sources)\n");
  return 0;
}

#else

#include "cli/Options.h"
#include "core/Configuration.h"
#include "core/LocalFileOperations.h"
#include "gui/DesktopIntegration.h"
#include "gui/GuiCommandLine.h"
#include "gui/Icons.h"
#include "gui/SingleInstance.h"
#include "gui/SystemActions.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QImage>
#include <QLocalSocket>
#include <QMainWindow>
#include <QPainter>
#include <QPalette>
#include <QProcess>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <cstdio>
#include <functional>
#include <sys/stat.h>
#include <unistd.h>

static int Failures = 0;
static QByteArray OriginalRuntimeDir;  // the session's (Wayland socket, D-Bus)
static int Checks = 0;

#define CHECK(Cond) \
  do { Checks++; if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_MSG(Cond, Msg) \
  do { Checks++; if (!(Cond)) { std::printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, #Cond, qPrintable(Msg)); std::fflush(stdout); Failures++; } } while (0)

static bool WaitFor(const std::function<bool()> & Condition, int Ms = 10000)
{
  QElapsedTimer Timer;
  Timer.start();
  while (!Condition())
  {
    if (Timer.elapsed() > Ms)
    {
      return false;
    }
    QApplication::processEvents(QEventLoop::AllEvents, 10);
    QThread::msleep(10);
  }
  return true;
}

static void WriteFile(const QString & Path, const QByteArray & Data, bool Executable = false)
{
  QFile F(Path);
  if (F.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    F.write(Data);
  }
  F.close();
  if (Executable)
  {
    F.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
  }
}

static QByteArray ReadFile(const QString & Path)
{
  QFile F(Path);
  return F.open(QIODevice::ReadOnly) ? F.readAll() : QByteArray();
}

static QString SourceDir()
{
  return QFileInfo(QStringLiteral(__FILE__)).absoluteDir().absoluteFilePath(QStringLiteral(".."));
}

//---------------------------------------------------------------------------
// Icons

static QStringList IconSearchPaths()
{
  QStringList Paths{QDir::homePath() + QStringLiteral("/.local/share/icons"), QStringLiteral("/usr/share/icons")};
  Paths += qEnvironmentVariable("NIXSCP_VERIFY_ICON_PATHS").split(QLatin1Char(':'), Qt::SkipEmptyParts);
  return Paths;
}

static bool ThemeInstalled(const QString & Theme)
{
  for (const QString & Path : IconSearchPaths())
  {
    if (QFileInfo::exists(Path + QLatin1Char('/') + Theme + QStringLiteral("/index.theme")))
    {
      return true;
    }
  }
  return false;
}

// Child: resolves every icon in Theme; prints "MISSING <name>" / "STYLE <name>"
static int IconThemeChild(const QString & Theme, const QString & SheetDir)
{
  QIcon::setThemeSearchPaths(IconSearchPaths());
  QIcon::setThemeName(Theme);
  QIcon::setFallbackThemeName(QStringLiteral("hicolor"));
  struct Row { QString Name; QString ThemeName; QIcon Icon; };
  QList<Row> Rows;
  int Bad = 0;
  for (const QString & Name : Icons::FallbackNames())
  {
    Row R{Name, Icons::FallbackThemeIconName(Name), Icons::Get(Name)};
    if (R.Icon.isNull() || R.Icon.pixmap(16).isNull())
    {
      std::printf("MISSING %s\n", qPrintable(Name));
      Bad++;
    }
    else if (R.ThemeName.isEmpty())
    {
      std::printf("STYLE %s\n", qPrintable(Name));
      Bad++;
    }
    Rows.append(R);
  }
  for (const QString & Name : Icons::FallbackDialogImageNames())
  {
    Row R{QStringLiteral("dialog:") + Name, Icons::FallbackThemeIconName(Name, true), Icons::DialogImage(Name)};
    if (R.Icon.isNull() || R.Icon.pixmap(32).isNull())
    {
      std::printf("MISSING %s\n", qPrintable(R.Name));
      Bad++;
    }
    else if (R.ThemeName.isEmpty())
    {
      std::printf("STYLE %s\n", qPrintable(R.Name));
      Bad++;
    }
    Rows.append(R);
  }
  std::printf("THEME %s %d icons %d unresolved\n", qPrintable(QIcon::themeName()), int(Rows.size()), Bad);

  if (!SheetDir.isEmpty())
  {
    // Contact sheet: 16 and 24 px on a light and a dark background (symbolic
    // icons of Adwaita/Yaru are dark grey and may vanish on dark themes)
    const int Columns = 3;
    const int CellW = 330;
    const int CellH = 30;
    int RowCount = static_cast<int>((Rows.size() + Columns - 1) / Columns);
    QImage Sheet(Columns * CellW, RowCount * CellH + 24, QImage::Format_ARGB32);
    Sheet.fill(Qt::white);
    QPainter Painter(&Sheet);
    QFont Font = Painter.font();
    Font.setPixelSize(10);
    Painter.setFont(Font);
    Painter.drawText(4, 16, QStringLiteral("Icon theme %1 (NixSCP fallback icons: name / resolved freedesktop name)").arg(Theme));
    for (int I = 0; I < Rows.size(); I++)
    {
      int X = (I % Columns) * CellW;
      int Y = 24 + (I / Columns) * CellH;
      Painter.drawPixmap(X + 2, Y + 7, Rows[I].Icon.pixmap(16));
      Painter.drawPixmap(X + 22, Y + 3, Rows[I].Icon.pixmap(24));
    }
    // the same icons with a dark palette (symbolic icons follow the text color)
    QPalette Light = QApplication::palette();
    QPalette Dark = Light;
    Dark.setColor(QPalette::Window, QColor(0x2b, 0x2b, 0x2b));
    Dark.setColor(QPalette::WindowText, QColor(0xee, 0xee, 0xee));
    QApplication::setPalette(Dark);
    for (int I = 0; I < Rows.size(); I++)
    {
      int X = (I % Columns) * CellW;
      int Y = 24 + (I / Columns) * CellH;
      Painter.fillRect(X + 56, Y, 52, CellH - 2, QColor(0x2b, 0x2b, 0x2b));
      Painter.drawPixmap(X + 60, Y + 7, Rows[I].Icon.pixmap(16));
      Painter.drawPixmap(X + 80, Y + 3, Rows[I].Icon.pixmap(24));
    }
    QApplication::setPalette(Light);
    for (int I = 0; I < Rows.size(); I++)
    {
      int X = (I % Columns) * CellW;
      int Y = 24 + (I / Columns) * CellH;
      Painter.setPen(Rows[I].ThemeName.isEmpty() ? Qt::red : Qt::black);
      Painter.drawText(X + 112, Y + 12, Rows[I].Name);
      Painter.setPen(Qt::darkGray);
      Painter.drawText(X + 112, Y + 25, Rows[I].ThemeName.isEmpty() ? QStringLiteral("(no theme icon)") : Rows[I].ThemeName);
    }
    Painter.end();
    QDir().mkpath(SheetDir);
    Sheet.save(QDir(SheetDir).filePath(Theme + QStringLiteral(".png")));
  }
  return (Bad == 0) ? 0 : 1;
}

// Child: no platform theme, theme name from the desktop configuration
static int DetectThemeChild()
{
  Icons::FromTheme(QStringLiteral("folder"));
  std::printf("DETECTED %s\n", qPrintable(QIcon::themeName()));
  return 0;
}

static void TestIconThemes(const QString & Scratch)
{
  std::printf("- icon themes\n");
  const QStringList Themes{QStringLiteral("breeze"), QStringLiteral("Adwaita"), QStringLiteral("Yaru"), QStringLiteral("elementary-xfce")};
  for (const QString & Theme : Themes)
  {
    if (!ThemeInstalled(Theme))
    {
      std::printf("  SKIP %s: not installed (set NIXSCP_VERIFY_ICON_PATHS)\n", qPrintable(Theme));
      continue;
    }
    QProcess Child;
    Child.setProcessChannelMode(QProcess::MergedChannels);
    Child.start(QCoreApplication::applicationFilePath(),
      {QStringLiteral("--icon-theme"), Theme, qEnvironmentVariable("NIXSCP_VERIFY_CONTACT_SHEETS")});
    CHECK(Child.waitForFinished(60000));
    QString Output = QString::fromLocal8Bit(Child.readAll());
    std::printf("  %s: %s\n", qPrintable(Theme), qPrintable(Output.trimmed().split(QLatin1Char('\n')).last()));
    CHECK_MSG((Child.exitStatus() == QProcess::NormalExit) && (Child.exitCode() == 0), Theme + QStringLiteral(":\n") + Output);
  }

  // Desktop configuration without a platform theme plugin
  QString IconRoot;
  for (const QString & Path : IconSearchPaths())
  {
    if (QFileInfo::exists(Path + QStringLiteral("/elementary-xfce/index.theme")))
    {
      IconRoot = Path;
      break;
    }
  }
  struct Case { const char * File; const char * Contents; const char * Theme; };
  const Case Cases[] =
  {
    {"xfce4/xfconf/xfce-perchannel-xml/xsettings.xml",
     "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<channel name=\"xsettings\" version=\"1.0\">\n"
     "  <property name=\"Net\" type=\"empty\">\n"
     "    <property name=\"ThemeName\" type=\"string\" value=\"Greybird\"/>\n"
     "    <property name=\"IconThemeName\" type=\"string\" value=\"elementary-xfce\"/>\n"
     "  </property>\n</channel>\n", "elementary-xfce"},
    {"kdeglobals", "[General]\nColorScheme=BreezeLight\n\n[Icons]\nTheme=elementary-xfce\n", "elementary-xfce"},
  };
  if (IconRoot.isEmpty() || !IconRoot.endsWith(QStringLiteral("/icons")))
  {
    std::printf("  SKIP theme detection: elementary-xfce not installed\n");
    return;
  }
  int Index = 0;
  for (const Case & C : Cases)
  {
    QString Config = Scratch + QStringLiteral("/detect%1").arg(Index++);
    QString FileName = Config + QLatin1Char('/') + QString::fromLatin1(C.File);
    QDir().mkpath(QFileInfo(FileName).absolutePath());
    WriteFile(FileName, C.Contents);
    QProcess Child;
    QProcessEnvironment Env = QProcessEnvironment::systemEnvironment();
    Env.insert(QStringLiteral("XDG_CONFIG_HOME"), Config);
    Env.insert(QStringLiteral("XDG_DATA_DIRS"), IconRoot.chopped(6) + QStringLiteral(":/usr/share"));
    Env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    Env.remove(QStringLiteral("QT_QPA_PLATFORMTHEME"));
    Child.setProcessEnvironment(Env);
    Child.setProcessChannelMode(QProcess::MergedChannels);
    Child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--detect-theme")});
    CHECK(Child.waitForFinished(30000));
    QString Output = QString::fromLocal8Bit(Child.readAll()).trimmed();
    CHECK_MSG(Output.contains(QStringLiteral("DETECTED ") + QString::fromLatin1(C.Theme)), QString::fromLatin1(C.File) + QStringLiteral(": ") + Output);
  }
}

//---------------------------------------------------------------------------
// Terminal emulators

struct FakeTerminal
{
  const char * Name;
  const char * Script;      // body; $LOG is the call log
};

// Stand-ins with the real argument conventions: a wrong switch fails like
// the real program would (xfce4-terminal -e takes one string, gnome-terminal
// -e is deprecated and takes one string)
static const FakeTerminal FakeTerminals[] =
{
  {"gnome-terminal", "[ \"$1\" = \"--\" ] || { echo \"gnome-terminal bad $*\" >> \"$LOG\"; exit 2; }; shift"},
  {"ptyxis", "case \"$1\" in --|-e) shift;; *) echo \"ptyxis bad $*\" >> \"$LOG\"; exit 2;; esac"},
  {"kgx", "case \"$1\" in --|-e) shift;; *) echo \"kgx bad $*\" >> \"$LOG\"; exit 2;; esac"},
  {"konsole", "[ \"$1\" = \"-e\" ] || { echo \"konsole bad $*\" >> \"$LOG\"; exit 2; }; shift"},
  {"xfce4-terminal", "[ \"$1\" = \"-x\" ] || { echo \"xfce4-terminal bad $*\" >> \"$LOG\"; exit 2; }; shift"},
  {"xterm", "[ \"$1\" = \"-e\" ] || { echo \"xterm bad $*\" >> \"$LOG\"; exit 2; }; shift"},
  {"xdg-terminal-exec", ":"},
  {"x-terminal-emulator", "[ \"$1\" = \"-e\" ] || { echo \"x-terminal-emulator bad $*\" >> \"$LOG\"; exit 2; }; shift"},
};

static void MakeFakeBin(const QString & Dir, const QStringList & Terminals, const QString & Log)
{
  QDir(Dir).removeRecursively();
  QDir().mkpath(Dir);
  for (const FakeTerminal & T : FakeTerminals)
  {
    if (Terminals.contains(QString::fromLatin1(T.Name)))
    {
      WriteFile(Dir + QLatin1Char('/') + QString::fromLatin1(T.Name),
        QStringLiteral("#!/bin/sh\nLOG='%1'\necho \"terminal %2\" >> \"$LOG\"\n%3\nexec \"$@\"\n")
          .arg(Log, QString::fromLatin1(T.Name), QString::fromLatin1(T.Script)).toUtf8(), true);
    }
  }
  // ssh: one argument per line
  WriteFile(Dir + QStringLiteral("/ssh"),
    QStringLiteral("#!/bin/sh\nLOG='%1'\necho ssh >> \"$LOG\"\nfor A in \"$@\"; do echo \"arg $A\" >> \"$LOG\"; done\n").arg(Log).toUtf8(), true);
}

static SessionData TerminalSession()
{
  SessionData Data;
  Data.Name = QStringLiteral("Terminal Site");
  Data.FSProtocol = fsSFTP;
  Data.HostName = QStringLiteral("example.com");
  Data.PortNumber = 2222;
  Data.UserName = QStringLiteral("joe");
  Data.RemoteDirectory = QStringLiteral("/home/joe/dir with space");
  return Data;
}

static void TestTerminals(const QString & Scratch)
{
  std::printf("- terminal emulators (Open in PuTTY)\n");
  Configuration & Config = Configuration::Instance();
  Config.PuttyPath = Configuration::DefaultPuttyPath();
  Config.PuttyPassword = false;
  QByteArray OldPath = qgetenv("PATH");
  QByteArray OldDesktop = qgetenv("XDG_CURRENT_DESKTOP");
  QByteArray OldTerminal = qgetenv("TERMINAL");
  struct Scenario { const char * Desktop; QStringList Installed; const char * Terminal; const char * Expected; };
  const QList<Scenario> Scenarios =
  {
    {"ubuntu:GNOME", {"x-terminal-emulator", "konsole"}, "", "x-terminal-emulator"},
    {"GNOME", {"gnome-terminal", "konsole", "xterm"}, "", "gnome-terminal"},
    {"GNOME", {"ptyxis", "gnome-terminal"}, "", "ptyxis"},
    {"ubuntu:GNOME", {"kgx", "xterm"}, "", "kgx"},
    {"KDE", {"gnome-terminal", "konsole"}, "", "konsole"},
    {"XFCE", {"xterm", "gnome-terminal", "xfce4-terminal"}, "", "xfce4-terminal"},
    {"X-Cinnamon", {"xterm"}, "", "xterm"},
    {"KDE", {"xdg-terminal-exec", "konsole"}, "", "xdg-terminal-exec"},
    {"GNOME", {"gnome-terminal", "konsole"}, "konsole", "konsole"},
  };
  int Index = 0;
  for (const Scenario & S : Scenarios)
  {
    QString Dir = Scratch + QStringLiteral("/termbin%1").arg(Index++);
    QString Log = Dir + QStringLiteral(".log");
    QFile::remove(Log);
    MakeFakeBin(Dir, S.Installed, Log);
    qputenv("PATH", Dir.toUtf8());
    qputenv("XDG_CURRENT_DESKTOP", S.Desktop);
    if (qstrlen(S.Terminal) > 0) qputenv("TERMINAL", S.Terminal); else qunsetenv("TERMINAL");
    CHECK(DesktopIntegration::OpenSessionInTerminal(nullptr, TerminalSession(), QString(), QStringLiteral("/home/joe/dir with space"), true));
    bool Done = WaitFor([&Log]() { return ReadFile(Log).contains("arg joe@example.com") || ReadFile(Log).contains(" bad "); }, 10000);
    QString Text = QString::fromUtf8(ReadFile(Log));
    QString Label = QStringLiteral("%1 %2: %3").arg(QString::fromLatin1(S.Desktop), S.Installed.join(QLatin1Char(',')), Text);
    CHECK_MSG(Done, Label);
    CHECK_MSG(Text.startsWith(QStringLiteral("terminal %1\n").arg(QString::fromLatin1(S.Expected))), Label);
    CHECK_MSG(!Text.contains(QStringLiteral(" bad ")), Label);
    CHECK_MSG(Text.contains(QStringLiteral("arg -p\narg 2222\n")), Label);
    CHECK_MSG(Text.contains(QStringLiteral("arg cd '/home/joe/dir with space' && exec \"$SHELL\" -l")), Label);
  }

  // ssh:// URL (ParseUrl PuttyProtocol) opens the terminal too
  {
    QString Dir = Scratch + QStringLiteral("/termbin-url");
    QString Log = Dir + QStringLiteral(".log");
    MakeFakeBin(Dir, {QStringLiteral("konsole")}, Log);
    qputenv("PATH", Dir.toUtf8());
    qputenv("XDG_CURRENT_DESKTOP", "KDE");
    qunsetenv("TERMINAL");
    Options Params;
    Params.Add(QStringLiteral("ssh://alice@ssh.example.org:2200/"));
    GuiCommandLine::LoginData Data = GuiCommandLine::GetLoginData(QStringLiteral("ssh://alice@ssh.example.org:2200/"), Params,
      GuiCommandLine::lnNone, nullptr, true);
    CHECK(Data.Aborted);
    bool Done = WaitFor([&Log]() { return ReadFile(Log).contains("arg alice@ssh.example.org") || ReadFile(Log).contains(" bad "); }, 10000);
    QString Text = QString::fromUtf8(ReadFile(Log));
    CHECK_MSG(Done && Text.startsWith(QStringLiteral("terminal konsole\n")) && Text.contains(QStringLiteral("arg -p\narg 2200\n")), Text);
  }

  qputenv("PATH", OldPath);
  if (OldDesktop.isNull()) qunsetenv("XDG_CURRENT_DESKTOP"); else qputenv("XDG_CURRENT_DESKTOP", OldDesktop);
  if (OldTerminal.isNull()) qunsetenv("TERMINAL"); else qputenv("TERMINAL", OldTerminal);
}

//---------------------------------------------------------------------------
// Trash

static void TestTrash(const QString & Scratch)
{
  std::printf("- trash\n");
  // XDG_DATA_HOME is <Scratch>/data (main): the home trash is on the scratch volume
  QString Dir = Scratch + QStringLiteral("/trash-src");
  QDir().mkpath(Dir + QStringLiteral("/folder/sub"));
  WriteFile(Dir + QStringLiteral("/file one.txt"), "one");
  WriteFile(Dir + QStringLiteral("/folder/sub/two.txt"), "two");
  QString Error;
  CHECK_MSG(LocalFileOperations::MoveToTrash(Dir + QStringLiteral("/file one.txt"), &Error), Error);
  CHECK_MSG(LocalFileOperations::MoveToTrash(Dir + QStringLiteral("/folder"), &Error), Error);
  CHECK(!QFileInfo::exists(Dir + QStringLiteral("/file one.txt")));
  CHECK(!QFileInfo::exists(Dir + QStringLiteral("/folder")));
  QString Trash = Scratch + QStringLiteral("/data/Trash");
  CHECK(ReadFile(Trash + QStringLiteral("/files/file one.txt")) == "one");
  CHECK(ReadFile(Trash + QStringLiteral("/files/folder/sub/two.txt")) == "two");
  QByteArray Info = ReadFile(Trash + QStringLiteral("/info/file one.txt.trashinfo"));
  CHECK_MSG(Info.contains("[Trash Info]") && Info.contains("DeletionDate="), QString::fromUtf8(Info));
  CHECK_MSG(Info.contains("Path=" + QUrl::toPercentEncoding(Dir + QStringLiteral("/file one.txt"), "/")), QString::fromUtf8(Info));
  CHECK(!LocalFileOperations::MoveToTrash(Dir + QStringLiteral("/does-not-exist"), &Error));
  CHECK(!Error.isEmpty());

  // A volume without a trash: a file on another file system than the home
  // trash whose top directory is not writable (no $topdir/.Trash-$uid)
  QString Other = QCoreApplication::applicationDirPath() + QStringLiteral("/verify-desktop-trash.txt");
  struct stat A, B, Root;
  if ((stat(qPrintable(Scratch), &A) == 0) && (stat(qPrintable(QCoreApplication::applicationDirPath()), &B) == 0) &&
      (stat("/", &Root) == 0) && (A.st_dev != B.st_dev) && (B.st_dev == Root.st_dev) && (access("/", W_OK) != 0))
  {
    WriteFile(Other, "x");
    Error.clear();
    CHECK(!LocalFileOperations::MoveToTrash(Other, &Error));
    CHECK_MSG(Error.contains(QStringLiteral("Cannot move")), Error);
    CHECK(QFileInfo::exists(Other));
    QFile::remove(Other);
  }
  else
  {
    std::printf("  SKIP trash-less volume: build directory not on a separate, read-only-root volume\n");
  }
}

//---------------------------------------------------------------------------
// Single instance

static int ListenRaceChild(const QString & Name)
{
  SingleInstance Instance(Name);
  bool Ok = Instance.Listen();
  std::printf("LISTEN %d\n", Ok ? 1 : 0);
  std::fflush(stdout);
  // keep listening until all instances tried (the parent creates <Name>.done)
  QElapsedTimer Timer;
  Timer.start();
  while (!QFileInfo::exists(Name + QStringLiteral(".done")) && (Timer.elapsed() < 60000))
  {
    QApplication::processEvents(QEventLoop::AllEvents, 50);
    QThread::msleep(20);
  }
  return 0;
}

static void TestSingleInstance(const QString & Scratch)
{
  std::printf("- single instance\n");
  QString Name = Scratch + QStringLiteral("/run/nixscp-race");
  QList<QProcess *> Children;
  for (int I = 0; I < 6; I++)
  {
    auto * Child = new QProcess();
    Child->setProcessChannelMode(QProcess::MergedChannels);
    Child->start(QCoreApplication::applicationFilePath(), {QStringLiteral("--listen-race"), Name});
    Children.append(Child);
  }
  int Listening = 0;
  for (QProcess * Child : Children)
  {
    QElapsedTimer Timer;
    Timer.start();
    QByteArray Output;
    while (!Output.contains("LISTEN") && (Timer.elapsed() < 60000) && (Child->state() != QProcess::NotRunning))
    {
      Child->waitForReadyRead(1000);
      Output += Child->readAll();
    }
    Listening += Output.count("LISTEN 1");
  }
  WriteFile(Name + QStringLiteral(".done"), "");
  for (QProcess * Child : Children)
  {
    Child->waitForFinished(20000);
    delete Child;
  }
  CHECK_MSG(Listening == 1, QStringLiteral("%1 instances listen").arg(Listening));

  // A runtime directory too long for a socket address (sun_path: 107 bytes)
  {
    QByteArray OldRuntime = qgetenv("XDG_RUNTIME_DIR");
    QString Long = Scratch + QStringLiteral("/run/") + QString(120, QLatin1Char('r'));
    QDir().mkpath(Long);
    QFile::setPermissions(Long, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    qputenv("XDG_RUNTIME_DIR", Long.toUtf8());
    QString LongName = SingleInstance::DefaultServerName(Scratch + QStringLiteral("/long.ini"));
    qputenv("XDG_RUNTIME_DIR", OldRuntime);
    CHECK_MSG(QFile::encodeName(LongName).size() <= 107, LongName);
    SingleInstance LongInstance(LongName);
    CHECK(LongInstance.Listen());
    QLocalSocket Probe;
    Probe.connectToServer(LongName);
    CHECK(Probe.waitForConnected(5000));
  }

  // sftp:// URL from a second instance, with the activation token
  QString Server = SingleInstance::DefaultServerName(Scratch + QStringLiteral("/nixscp.ini"));
  CHECK(Server.startsWith(qEnvironmentVariable("XDG_RUNTIME_DIR")));
  SingleInstance Instance(Server);
  CHECK(Instance.Listen());
  QStringList Received;
  QString Token;
  QObject::connect(&Instance, &SingleInstance::CommandLineReceived,
    [&Received, &Token](const QStringList & Arguments, const QString &, const QString & ActivationToken)
    {
      Received = Arguments;
      Token = ActivationToken;
    });
  QProcess Sender;
  QProcessEnvironment Env = QProcessEnvironment::systemEnvironment();
  Env.insert(QStringLiteral("XDG_ACTIVATION_TOKEN"), QStringLiteral("token-123"));
  Sender.setProcessEnvironment(Env);
  Sender.setProcessChannelMode(QProcess::MergedChannels);
  Sender.start(QCoreApplication::applicationFilePath(),
    {QStringLiteral("--send"), Server, QStringLiteral("/unsafe"), QStringLiteral("sftp://joe@example.com/home/")});
  CHECK(WaitFor([&Sender]() { return Sender.state() == QProcess::NotRunning; }, 20000));
  CHECK_MSG(Sender.readAll().contains("SENT 1"), QStringLiteral("sender"));
  CHECK(WaitFor([&Received]() { return !Received.isEmpty(); }, 5000));
  CHECK(Received == (QStringList{QStringLiteral("/unsafe"), QStringLiteral("sftp://joe@example.com/home/")}));
  CHECK(Token == QStringLiteral("token-123"));
}

static int SendChild(const QString & Server, const QStringList & Arguments)
{
  bool Ok = SingleInstance::SendCommandLine(Server, Arguments, QDir::currentPath());
  std::printf("SENT %d\n", Ok ? 1 : 0);
  return 0;
}

//---------------------------------------------------------------------------
// Tray, notifications, file manager, sleep inhibitor

static void TestTrayUnavailable()
{
  std::printf("- tray icon availability\n");
  Configuration & Config = Configuration::Instance();
  Config.MinimizeToTray = true;
  QMainWindow Window;
  Window.resize(300, 200);
  DesktopIntegration::InstallMainWindow(&Window);
  Window.show();
  QApplication::processEvents();
  bool Available = QSystemTrayIcon::isSystemTrayAvailable();
  Window.showMinimized();
  WaitFor([]() { return false; }, 300);
  if (!Available)
  {
    // no StatusNotifierWatcher/XEmbed tray (GNOME without AppIndicator
    // extension, offscreen): a plain minimize, the window must stay reachable
    CHECK(!DesktopIntegration::IsMinimizedToTray());
    CHECK(Window.isVisible());
  }
  else
  {
    CHECK(DesktopIntegration::IsMinimizedToTray());
    DesktopIntegration::RestoreFromTray();
  }
  Config.MinimizeToTray = false;
}

static void TestNotifyAndExplore(const QString & Scratch)
{
  std::printf("- notifications and file manager (gdbus stand-in)\n");
  QString Dir = Scratch + QStringLiteral("/dbusbin");
  QString Log = Dir + QStringLiteral(".log");
  QDir().mkpath(Dir);
  WriteFile(Dir + QStringLiteral("/gdbus"),
    QStringLiteral("#!/bin/sh\nfor A in \"$@\"; do printf '%s|' \"$A\" >> '%1'; done\necho >> '%1'\n"
                   "case \"$*\" in *FileManager1*) exit 1;; esac\nexit 0\n").arg(Log).toUtf8(), true);
  WriteFile(Dir + QStringLiteral("/xdg-open"),
    QStringLiteral("#!/bin/sh\necho \"xdg-open $*\" >> '%1'\n").arg(Log).toUtf8(), true);
  QByteArray OldPath = qgetenv("PATH");
  qputenv("PATH", Dir.toUtf8() + ":/usr/bin:/bin");
  DesktopIntegration::SetCommandRunner(nullptr);
  Configuration::Instance().BalloonNotifications = true;

  SystemActions::Notify(QStringLiteral("NixSCP"), QStringLiteral("Operation was completed"));
  CHECK(WaitFor([&Log]() { return ReadFile(Log).contains("Notifications.Notify"); }, 5000));
  QString Text = QString::fromUtf8(ReadFile(Log));
  CHECK_MSG(Text.contains(QStringLiteral("'NixSCP'|0|'nixscp'|'NixSCP'|'Operation was completed'|[]|{'desktop-entry': <'nixscp'>}|")), Text);

  // Explore Directory: FileManager1 fails (stand-in exits 1) -> xdg-open on the folder
  QString File = Scratch + QStringLiteral("/explore me.txt");
  WriteFile(File, "x");
  SystemActions::ShowFile(File);
  CHECK(WaitFor([&Log]() { return ReadFile(Log).contains("FileManager1.ShowItems"); }, 5000));
  Text = QString::fromUtf8(ReadFile(Log));
  CHECK_MSG(Text.contains(QStringLiteral("['") + QUrl::fromLocalFile(File).toString(QUrl::FullyEncoded) + QStringLiteral("']")), Text);
  if (QGuiApplication::platformName() == QStringLiteral("offscreen"))
  {
    // offscreen Qt has no URL services (QDesktopServices::openUrl fails);
    // the live-session check runs in a child on the desktop's platform
    std::printf("  xdg-open fallback: needs a display (see --explore child)\n");
  }
  else
  {
    CHECK(WaitFor([&Log]() { return ReadFile(Log).contains("xdg-open"); }, 5000));
  }
  qputenv("PATH", OldPath);
}

// Child on the live platform (wayland/xcb, no window is shown): FileManager1
// missing -> xdg-open of the folder
static int ExploreChild(const QString & File)
{
  SystemActions::ShowFile(File);
  WaitFor([]() { return false; }, 3000);
  return 0;
}

static void TestExploreLive(const QString & Scratch)
{
  QString Platform = !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") ? QStringLiteral("wayland") :
    (!qEnvironmentVariableIsEmpty("DISPLAY") ? QStringLiteral("xcb") : QString());
  if (Platform.isEmpty())
  {
    std::printf("  SKIP xdg-open fallback: no display\n");
    return;
  }
  QString Dir = Scratch + QStringLiteral("/dbusbin");
  QString Log = Dir + QStringLiteral(".log");
  QFile::remove(Log);
  QString File = Scratch + QStringLiteral("/explore me.txt");
  QProcess Child;
  QProcessEnvironment Env = QProcessEnvironment::systemEnvironment();
  Env.insert(QStringLiteral("PATH"), Dir + QStringLiteral(":/usr/bin:/bin"));
  Env.insert(QStringLiteral("QT_QPA_PLATFORM"), Platform);
  Env.insert(QStringLiteral("XDG_RUNTIME_DIR"), QString::fromLocal8Bit(OriginalRuntimeDir));
  Child.setProcessEnvironment(Env);
  Child.setProcessChannelMode(QProcess::MergedChannels);
  Child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--explore"), File});
  CHECK(Child.waitForFinished(30000));
  QString Text = QString::fromUtf8(ReadFile(Log));
  QString Folder = QUrl::fromLocalFile(QFileInfo(File).absolutePath()).toString(QUrl::FullyEncoded);
  CHECK_MSG(Text.contains(QStringLiteral("xdg-open ") + Folder), Platform + QStringLiteral(": ") + Text + QString::fromLocal8Bit(Child.readAll()));
}

static void TestSleepInhibitor()
{
  std::printf("- sleep inhibitor (logind)\n");
  QString Inhibit = QStandardPaths::findExecutable(QStringLiteral("systemd-inhibit"));
  if (Inhibit.isEmpty() || !QFileInfo::exists(QStringLiteral("/run/systemd/seats")))
  {
    std::printf("  SKIP: no systemd-logind\n");
    return;
  }
  {
    // Containers may have the logind directories but no system bus
    QProcess Probe;
    Probe.start(Inhibit, {QStringLiteral("--list"), QStringLiteral("--no-pager")});
    if (!Probe.waitForFinished(10000) || (Probe.exitStatus() != QProcess::NormalExit) || (Probe.exitCode() != 0))
    {
      std::printf("  SKIP: systemd-logind not reachable\n");
      return;
    }
  }
  auto Listed = [&Inhibit]()
  {
    QProcess P;
    P.start(Inhibit, {QStringLiteral("--list"), QStringLiteral("--no-pager")});
    P.waitForFinished(10000);
    return QString::fromLocal8Bit(P.readAllStandardOutput()).contains(QStringLiteral("Transfer in progress"));
  };
  if (Listed())
  {
    std::printf("  SKIP: another NixSCP holds a lock\n");
    return;
  }
  SystemActions::SleepInhibitor::SetBackendEnabled(true);
  SystemActions::SleepInhibitor::Acquire();
  CHECK(SystemActions::SleepInhibitor::LockActive());
  CHECK(WaitFor([&Listed]() { return Listed(); }, 10000));
  SystemActions::SleepInhibitor::Release();
  CHECK(!SystemActions::SleepInhibitor::Held());
  CHECK(WaitFor([&Listed]() { return !Listed(); }, 10000));
}

//---------------------------------------------------------------------------
// Desktop entries

static bool ValidateDesktopFile(const QString & Validator, const QString & FileName, QString & Output)
{
  QProcess P;
  P.setProcessChannelMode(QProcess::MergedChannels);
  P.start(Validator, {FileName});
  P.waitForFinished(30000);
  Output = QString::fromLocal8Bit(P.readAll()).trimmed();
  return (P.exitStatus() == QProcess::NormalExit) && (P.exitCode() == 0) && !Output.contains(QStringLiteral("error:"));
}

static void TestDesktopEntries(const QString & Scratch)
{
  std::printf("- desktop entries\n");
  QString Validator = QStandardPaths::findExecutable(QStringLiteral("desktop-file-validate"));
  if (Validator.isEmpty())
  {
    std::printf("  SKIP: desktop-file-validate not installed\n");
    return;
  }
  QString Output;
  QString Shipped = SourceDir() + QStringLiteral("/data/nixscp.desktop");
  CHECK_MSG(ValidateDesktopFile(Validator, Shipped, Output), Output);
  QByteArray Contents = ReadFile(Shipped);
  CHECK(Contents.contains("Exec=nixscp /unsafe %u"));
  CHECK(Contents.contains("x-scheme-handler/sftp;"));
  CHECK(Contents.contains("StartupWMClass=nixscp"));

  // Desktop Icon launchers (names with spaces, quotes, $ and %)
  struct Case { const char * Name; DesktopIntegration::ShortCutKind Kind; };
  const Case Cases[] =
  {
    {"Folder/My \"Site\" $HOME 100%", DesktopIntegration::sscSite},
    {"Work Sites", DesktopIntegration::sscFolder},
    {"My Workspace", DesktopIntegration::sscWorkspace},
  };
  int Index = 0;
  for (const Case & C : Cases)
  {
    QString FileName = Scratch + QStringLiteral("/launcher%1.desktop").arg(Index++);
    WriteFile(FileName, DesktopIntegration::DesktopEntry(QString::fromUtf8(C.Name), C.Kind, QString(),
      QStringLiteral("/opt/Lin SCP/nixscp")).toUtf8());
    CHECK_MSG(ValidateDesktopFile(Validator, FileName, Output), Output);
  }

  // /RegisterForDefaultProtocols (XDG_DATA_HOME / XDG_CONFIG_HOME are scratch)
  QString Error;
  CHECK_MSG(GuiCommandLine::RegisterForDefaultProtocols(&Error), Error);
  QString Handler = GuiCommandLine::UrlHandlerDesktopFileName();
  CHECK(Handler.startsWith(Scratch));
  CHECK_MSG(ValidateDesktopFile(Validator, Handler, Output), Output);
  if (!QStandardPaths::findExecutable(QStringLiteral("xdg-mime")).isEmpty())
  {
    QProcess P;
    P.start(QStringLiteral("xdg-mime"), {QStringLiteral("query"), QStringLiteral("default"), QStringLiteral("x-scheme-handler/sftp")});
    P.waitForFinished(10000);
    CHECK_MSG(P.readAllStandardOutput().trimmed() == "nixscp-url-handler.desktop", QStringLiteral("xdg-mime query"));
  }
  CHECK(GuiCommandLine::UnregisterForProtocols(&Error));
  CHECK(!QFileInfo::exists(Handler));
}

//---------------------------------------------------------------------------

int main(int argc, char ** argv)
{
  QString Mode = (argc > 1) ? QString::fromLocal8Bit(argv[1]) : QString();
  if (Mode == QStringLiteral("--explore"))
  {
    QApplication App(argc, argv);
    return ExploreChild(QString::fromLocal8Bit(argv[2]));
  }
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
  {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  if (Mode == QStringLiteral("--icon-theme"))
  {
    QApplication App(argc, argv);
    return IconThemeChild(QString::fromLocal8Bit(argv[2]), (argc > 3) ? QString::fromLocal8Bit(argv[3]) : QString());
  }
  if (Mode == QStringLiteral("--detect-theme"))
  {
    QApplication App(argc, argv);
    return DetectThemeChild();
  }
  if (Mode == QStringLiteral("--listen-race"))
  {
    QApplication App(argc, argv);
    return ListenRaceChild(QString::fromLocal8Bit(argv[2]));
  }
  if (Mode == QStringLiteral("--send"))
  {
    QCoreApplication App(argc, argv);
    QStringList Arguments;
    for (int I = 3; I < argc; I++)
    {
      Arguments.append(QString::fromLocal8Bit(argv[I]));
    }
    return SendChild(QString::fromLocal8Bit(argv[2]), Arguments);
  }

  OriginalRuntimeDir = qgetenv("XDG_RUNTIME_DIR");
  QTemporaryDir Scratch;
  QString Path = Scratch.path();
  qputenv("XDG_CONFIG_HOME", (Path + QStringLiteral("/config")).toUtf8());
  qputenv("XDG_DATA_HOME", (Path + QStringLiteral("/data")).toUtf8());
  qputenv("XDG_RUNTIME_DIR", (Path + QStringLiteral("/run")).toUtf8());
  QDir().mkpath(Path + QStringLiteral("/run"));
  QDir().mkpath(Path + QStringLiteral("/data"));
  QFile::setPermissions(Path + QStringLiteral("/run"), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
  QApplication App(argc, argv);
  Configuration::Instance().SetIniFileName(Path + QStringLiteral("/nixscp.ini"));
  Configuration::Instance().Load();

  TestIconThemes(Path);
  TestTerminals(Path);
  TestTrash(Path);
  TestSingleInstance(Path);
  TestTrayUnavailable();
  TestNotifyAndExplore(Path);
  TestExploreLive(Path);
  TestSleepInhibitor();
  TestDesktopEntries(Path);

  std::printf("%d checks, %d failures\n", Checks, Failures);
  return (Failures == 0) ? 0 : 1;
}

#endif
