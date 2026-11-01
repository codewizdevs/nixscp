// Port of WinSCP source/windows/WinMain.cpp (Execute) and
// windows/UserInterface.cpp (CreateConfiguration): the GUI program
//
// nixscp [site|site folder|workspace|URL] [/upload file1 file2...]
//        [/synchronize [local] [remote]] [/keepuptodate [local] [remote]]
//        [/refresh [path]] [/browse[=file]] [/defaults[=secs]] [/newinstance]
//        [/privatekey=key] [/hostkey=fingerprint] [/passive[=on|off]]
//        [/rawsettings setting=value...] [/log=file] [/loglevel=n] [/logsize=[n*]size]
//        [/xmllog=file] [/ini=file|nul] [/rawconfig config=value...]
//        [/rawtransfersettings setting=value...] [/unsafe]
//        [/console|/script=file|/command ...|/help|/info|...] (run nixscp-cli)
// The rest of the command line logic lives in gui/GuiCommandLine and
// gui/SingleInstance.
#include "cli/Options.h"
#include "core/Configuration.h"
#include "core/FileSystem.h"
#include "core/IniStorage.h"
#include "core/RemoteFile.h"
#include "core/Workspaces.h"
#include "gui/CommanderWindow.h"
#include "gui/GuiCommandLine.h"
#include "gui/GuiUserInterface.h"
#include "gui/Icons.h"
#include "gui/SingleInstance.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QTimer>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <libssh/libssh.h>

using namespace GuiCommandLine;

// SSH_ASKPASS helper for "pass password to PuTTY": ssh runs this binary with the
// prompt as argument; answer password/passphrase prompts from the one-time file
static int AskPass(const char * FileName, const char * Prompt)
{
  QString Text = QString::fromLocal8Bit(Prompt ? Prompt : "").toLower();
  QFile File(QString::fromLocal8Bit(FileName));
  if (!Text.contains(QStringLiteral("password")) && !Text.contains(QStringLiteral("passphrase")))
  {
    // e.g. host key confirmation: not ours to answer
    return 1;
  }
  // "user@host's password:": with a jump host (-J) the jump host asks
  // first; never hand it the target's password
  QString Host = QString::fromLocal8Bit(qgetenv("NIXSCP_ASKPASS_HOST")).toLower();
  if (Text.contains(QStringLiteral("password")) && !Host.isEmpty() && !Text.contains(Host))
  {
    return 1;
  }
  if (!File.open(QIODevice::ReadOnly))
  {
    return 1;
  }
  QByteArray Password = File.readAll();
  File.close();
  File.remove();
  fwrite(Password.constData(), 1, static_cast<size_t>(Password.size()), stdout);
  fputc('\n', stdout);
  return 0;
}

// WinConfiguration->AutoStartSession (Configuration\Interface\AutoStartSession):
// site opened when no session is given on the command line
static QString AutoStartSessionName(Configuration & Config)
{
  IniStorage Storage(&Config.Ini());
  QString Result;
  if (Storage.OpenSubKey(QStringLiteral("Configuration"), false))
  {
    if (Storage.OpenSubKey(QStringLiteral("Interface"), false))
    {
      Result = Storage.ReadString(QStringLiteral("AutoStartSession"), QString());
      Storage.CloseSubKey();
    }
    Storage.CloseSubKey();
  }
  return Result;
}

// OpenInNewWindow: Shift held while the link was opened
static bool OpenInNewWindow()
{
  return (QGuiApplication::queryKeyboardModifiers() & Qt::ShiftModifier) != 0;
}

int main(int argc, char * argv[])
{
  if (const char * AskPassFile = getenv("NIXSCP_ASKPASS_FILE"))
  {
    if (getenv("SSH_ASKPASS") != nullptr)
    {
      return AskPass(AskPassFile, (argc > 1) ? argv[1] : nullptr);
    }
  }
  QStringList RawArgs;
  for (int I = 1; I < argc; I++)
  {
    RawArgs.append(QString::fromLocal8Bit(argv[I]));
  }
  if (RawArgs.contains(QStringLiteral("--version")))
  {
    printf("nixscp %s\n", PROJECT_VERSION);
    return 0;
  }
  // Console modes (/help, /console, /script...) need no GUI: nixscp-cli
  int ConsoleResult = RunConsoleMode(RawArgs);
  if (ConsoleResult >= 0)
  {
    return ConsoleResult;
  }

  ssh_init();
  QApplication App(argc, argv);
  QApplication::setApplicationName(QStringLiteral("NixSCP"));
  QApplication::setApplicationVersion(QStringLiteral(PROJECT_VERSION));
  QApplication::setOrganizationName(QStringLiteral("NixSCP"));
  QApplication::setDesktopFileName(QStringLiteral("nixscp"));
  QApplication::setWindowIcon(Icons::Application());

  // Qt has removed its own options (-platform...) by now
  QStringList Args = App.arguments().mid(1);
  Options Params;
  for (const QString & Arg : Args)
  {
    Params.Add(Arg);
  }

  // CreateConfiguration: /ini=<file|nul>, /rawconfig
  Configuration & Config = Configuration::Instance();
  QString IniFileMissing;
  ApplyIniSwitch(Params, &IniFileMissing);
  Config.Load();
  ApplyRawConfig(Params);
  if (!IniFileMissing.isEmpty())
  {
    // FILE_NOT_EXISTS
    ShowErrorMessage(nullptr, QStringLiteral("File or folder '%1' does not exist.").arg(IniFileMissing));
  }
  QString KeyFile;
  if (Params.FindSwitch(QStringLiteral("privatekey"), KeyFile))
  {
    SetDefaultKeyFile(KeyFile);
  }
  ApplyLoggingAndTransferSwitches(Params);

  // Maintenance tasks
  if (Params.FindSwitch(QStringLiteral("RegisterForDefaultProtocols")) ||
      Params.FindSwitch(QStringLiteral("RegisterAsUrlHandler"))) // BACKWARD COMPATIBILITY
  {
    QString Error;
    int Result = 0;
    if (CheckSafe(Params) && !RegisterForDefaultProtocols(&Error))
    {
      fprintf(stderr, "%s\n", qPrintable(Error));
      Result = 1;
    }
    ssh_finalize();
    return Result;
  }
  if (Params.FindSwitch(QStringLiteral("UnregisterForProtocols")))
  {
    QString Error;
    int Result = 0;
    if (CheckSafe(Params) && !UnregisterForProtocols(&Error))
    {
      fprintf(stderr, "%s\n", qPrintable(Error));
      Result = 1;
    }
    ssh_finalize();
    return Result;
  }

  ParamCommand Command = pcNone;
  QStringList CommandParams;
  int UseDefaults = -1;
  if (!Params.Empty())
  {
    QString Value;
    if (Params.FindSwitch(QStringLiteral("defaults"), Value) && CheckSafe(Params))
    {
      UseDefaults = Value.toInt();
    }
    if (Params.FindSwitch(QStringLiteral("upload"), CommandParams))
    {
      Command = pcUpload;
      if (CommandParams.isEmpty())
      {
        ShowErrorMessage(nullptr, QStringLiteral("No upload file list specified."));
        ssh_finalize();
        return 1;
      }
    }
    if (Params.FindSwitch(QStringLiteral("uploadifany"), CommandParams))
    {
      if (!CommandParams.isEmpty())
      {
        Command = pcUpload;
      }
    }
    else if (Params.FindSwitch(QStringLiteral("synchronize"), CommandParams, 4))
    {
      Command = pcFullSynchronize;
    }
    else if (Params.FindSwitch(QStringLiteral("keepuptodate"), CommandParams, 4))
    {
      Command = pcSynchronize;
    }
    else if (Params.FindSwitch(QStringLiteral("Edit"), CommandParams, 1) && (CommandParams.size() == 1))
    {
      Command = pcEdit;
    }
    else if (Params.FindSwitch(QStringLiteral("refresh"), CommandParams, 1))
    {
      Command = pcRefresh;
    }
  }

  StoredSessionList & Stored = Config.StoredSessions();
  QString ServerName = SingleInstance::DefaultServerName(Config.IsNulStorage() ? QStringLiteral("nul") : Config.IniFileName());
  bool NewInstance = Params.FindSwitch(QStringLiteral("newinstance"));
  bool Unsafe = !CheckSafe(Params);
  bool IsDownloadFile = false;
  QString AutoStartSession;
  if (Params.ParamCount() > 0)
  {
    AutoStartSession = Params.Param(1);
    if (!AutoStartSession.isEmpty() && AutoStartSession.contains(QLatin1Char('/')) &&
        FolderOrWorkspaceName(AutoStartSession, &Params, true, Stored).isEmpty())
    {
      // Make copy, as parsing consumes /rawsettings
      Options Copy(Params);
      try
      {
        IsDownloadFile = !ParseUrl(AutoStartSession, &Copy, Stored, Unsafe).DownloadFile.isEmpty();
      }
      catch (const ExtException &)
      {
        // reported below
      }
    }
    bool TrySendToAnotherInstance =
      (Command == pcNone) &&
      !IsDownloadFile &&
      (Config.ExternalSessionInExistingInstance != OpenInNewWindow()) &&
      !NewInstance &&
      // the other instance would not have the same configuration
      !Params.FindSwitch(QStringLiteral("rawconfig"));
    if (TrySendToAnotherInstance &&
        SingleInstance::SendCommandLine(ServerName, Args, QDir::currentPath()))
    {
      ssh_finalize();
      return 0;
    }
  }
  else if (NewInstance)
  {
    // no autostart
  }
  else
  {
    AutoStartSession = AutoStartSessionName(Config);
  }

  if (Command == pcRefresh)
  {
    SingleInstance::SendRefresh(ServerName, AutoStartSession, CommandParams.value(0));
    ssh_finalize();
    return 0;
  }

  bool IsCommandLineOperation = (Command != pcNone) || IsDownloadFile;
  LoginNeed Need = IsCommandLineOperation ? lnTerminal : (NewInstance ? lnSession : lnNone);
  LoginData Data;
  try
  {
    Data = GetLoginData(AutoStartSession, Params, Need, nullptr, Unsafe);
  }
  catch (const ExtException & E)
  {
    ShowErrorMessage(nullptr, E.Message(), E.MoreMessages());
    ssh_finalize();
    return 1;
  }
  if (Data.Aborted)
  {
    ssh_finalize();
    return 0;
  }
  ApplyLogSwitches(Params);

  if (!Data.Workspace.isEmpty() && Data.Sessions.isEmpty())
  {
    Data.Sessions = Workspaces::FolderOrWorkspace(Stored, Data.Workspace);
  }
  bool HaveSession = !Data.Sessions.isEmpty();

  // /browse[=file] (/explore): select the file instead of downloading it
  QString ExploreFileName;
  bool Explore = false;
  if (HaveSession &&
      (Params.FindSwitch(QStringLiteral("browse"), ExploreFileName) || Params.FindSwitch(QStringLiteral("explore"), ExploreFileName)) &&
      (!ExploreFileName.isEmpty() || !Data.DownloadFile.isEmpty()))
  {
    if (ExploreFileName.isEmpty())
    {
      ExploreFileName = Data.DownloadFile;
    }
    Data.DownloadFile.clear();
    Explore = true;
  }
  if (HaveSession && (Command == pcEdit))
  {
    // The standalone editor is not available: open the session at the file instead
    ExploreFileName = CommandParams.value(0);
    Explore = true;
  }
  if (Explore && ExploreFileName.startsWith(QLatin1Char('/')))
  {
    Data.Sessions.first().RemoteDirectory = UnixPath::ExtractFileDir(ExploreFileName);
    Data.Workspace.clear();
  }

  int Result = 0;
  if (HaveSession && ((Command == pcUpload) || (Command == pcFullSynchronize) || (Command == pcSynchronize) ||
                      !Data.DownloadFile.isEmpty()))
  {
    // StandaloneOperation: no main window, the application ends with the operation
    SessionData Session = Data.Sessions.first();
    CommandLineOperation::Kind Kind = CommandLineOperation::okUpload;
    QStringList OperationParams = CommandParams;
    if (!Data.DownloadFile.isEmpty())
    {
      Kind = CommandLineOperation::okDownload;
      OperationParams = QStringList{UnixPath::Combine(Session.RemoteDirectory.isEmpty() ? QStringLiteral("/") : Session.RemoteDirectory, Data.DownloadFile)};
      Session.RemoteDirectory.clear();
    }
    else if (Command == pcFullSynchronize)
    {
      Kind = CommandLineOperation::okFullSynchronize;
    }
    else if (Command == pcSynchronize)
    {
      Kind = CommandLineOperation::okKeepUpToDate;
    }
    CommandLineOperation Operation(Kind, Session, OperationParams, UseDefaults);
    Operation.Finished = [&Result](int ExitCode)
    {
      Result = ExitCode;
      QTimer::singleShot(0, qApp, []() { QApplication::quit(); });
    };
    QTimer::singleShot(0, &Operation, [&Operation]() { Operation.Start(); });
    App.setQuitOnLastWindowClosed(false);
    App.exec();
    ssh_finalize();
    return Result;
  }

  CommanderWindow Window;
  Window.show();

  // Sessions from other instances (ExternalSessionInExistingInstance), /refresh
  SingleInstance Instance(ServerName);
  Instance.SetCanAcceptCommandLine([&Window]() { return CanCommandLineFromAnotherInstance(&Window); });
  QObject::connect(&Instance, &SingleInstance::CommandLineReceived, &Window,
    [&Window](const QStringList & Arguments, const QString & WorkingDirectory, const QString & ActivationToken)
    {
      CommandLineFromAnotherInstance(&Window, Arguments, WorkingDirectory, ActivationToken);
    });
  QObject::connect(&Instance, &SingleInstance::RefreshRequested, &Window,
    [&Window](const QString & Session, const QString & Path) { RefreshPanel(&Window, Session, Path); });
  Instance.Listen();

  // "Show Login dialog ... when the last session is closed"
  new LastSessionWatcher(&Window);

  bool Screenshotting = !qEnvironmentVariableIsEmpty("NIXSCP_SCREENSHOT");
  QTimer::singleShot(0, &Window, [&Window, &Config, Data, Explore, ExploreFileName, Screenshotting]()
  {
    if (!Data.Sessions.isEmpty())
    {
      OpenSessions(&Window, Data);
      if (Explore)
      {
        ExploreFile(&Window, ExploreFileName);
      }
    }
    else if (Config.ShowLoginOnStartup && !Screenshotting)
    {
      // NeedSession(Startup)
      LoginData Login;
      if (DoLoginDialog(&Window, nullptr, true, Login))
      {
        OpenSessions(&Window, Login);
      }
    }
  });

  // Testing aid: NIXSCP_SCREENSHOT=<file.png> saves the main window and quits
  // (used with QT_QPA_PLATFORM=offscreen by the GUI verification scripts)
  if (Screenshotting)
  {
    QString Screenshot = qEnvironmentVariable("NIXSCP_SCREENSHOT");
    int Delay = qEnvironmentVariableIntValue("NIXSCP_SCREENSHOT_DELAY");
    QTimer::singleShot((Delay > 0) ? Delay : 800, &App, [&Window, Screenshot]()
    {
      Window.grab().save(Screenshot);
      QApplication::quit();
    });
  }

  Result = App.exec();
  ssh_finalize();
  return Result;
}
