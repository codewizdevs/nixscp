// Port of the GUI command line of WinSCP (windows/WinMain.cpp,
// windows/UserInterface.cpp, forms/Custom.cpp TSaveSessionDialog,
// forms/CustomScpExplorer.cpp, windows/Setup.cpp); see GuiCommandLine.h
#include "gui/GuiCommandLine.h"
#include "gui/HelpSystem.h"

// The command line tokenizer and session URL parser of nixscp-cli
// (TOptions, TSessionData::ParseUrl), shared with the GUI
#include "cli/Options.cpp"
#include "cli/SessionUrl.cpp"

#include "core/Configuration.h"
#include "core/CopyParam.h"
#include "core/FileMasks.h"
#include "core/IniStorage.h"
#include "core/RemoteFile.h"
#include "core/SessionLog.h"
#include "core/Terminal.h"
#include "core/Workspaces.h"
#include "gui/CommanderWindow.h"
#include "gui/CopyDialog.h"
#include "gui/DesktopIntegration.h"
#include "gui/FilePanel.h"
#include "gui/GuiUserInterface.h"
#include "gui/LoginDialog.h"
#include "gui/ProgressDialog.h"
#include "gui/SynchronizeDialogs.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QSettings>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>

#include <cstdio>
#include <unistd.h>

//---------------------------------------------------------------------------
// URL parsing (TStoredSessionList::ParseUrl for the command line)

namespace
{
  struct UrlScheme
  {
    const char * Name;
    TFSProtocol Protocol;
  };

  // DoIsProtocolUrl prefixes recognized by TSessionData::ParseUrl
  const UrlScheme UrlSchemeList[] = {
    {"scp", fsSCPonly}, {"sftp", fsSFTPonly}, {"ftp", fsFTP}, {"ftps", fsFTP}, {"ftpes", fsFTP},
    {"dav", fsWebDAV}, {"http", fsWebDAV}, {"davs", fsWebDAV}, {"https", fsWebDAV},
    {"s3plain", fsS3}, {"s3", fsS3}, {"ssh", fsSFTPonly},
  };

  bool CommandLineUrlProtocol(const QString & Url, QString & Scheme, TFSProtocol & Protocol, int & ProtocolLen)
  {
    for (const UrlScheme & S : UrlSchemeList)
    {
      QString Prefix = QString::fromLatin1(S.Name) + QLatin1Char(':');
      for (const QString & P : {Prefix, QStringLiteral("winscp-") + Prefix})
      {
        if (Url.startsWith(P, Qt::CaseInsensitive))
        {
          Scheme = QString::fromLatin1(S.Name);
          Protocol = S.Protocol;
          ProtocolLen = static_cast<int>(P.size());
          return true;
        }
      }
    }
    return false;
  }

  // The stored site lookup of TSessionData::ParseUrl: "site" or "site/path"
  std::shared_ptr<SessionData> MatchStoredSite(const StoredSessionList & Stored, const QString & DecodedUrl)
  {
    for (const auto & AData : Stored.Sessions())
    {
      if (AData->IsWorkspace)
      {
        continue;
      }
      const QString & Name = AData->Name;
      if (((Name.size() == DecodedUrl.size()) && (Name.compare(DecodedUrl, Qt::CaseInsensitive) == 0)) ||
          ((Name.size() < DecodedUrl.size()) && (DecodedUrl[Name.size()] == QLatin1Char('/')) &&
           DecodedUrl.startsWith(Name, Qt::CaseInsensitive)))
      {
        return AData;
      }
    }
    return nullptr;
  }

  // Removes the ";x-name=value" raw settings of the user info part of an ad-hoc URL
  QString StripUrlRawSettings(const QString & Url)
  {
    QString Scheme;
    TFSProtocol Protocol;
    int ProtocolLen = 0;
    qsizetype Start = 0;
    if (CommandLineUrlProtocol(Url, Scheme, Protocol, ProtocolLen))
    {
      Start = ProtocolLen;
      if (Url.mid(Start, 2) == QStringLiteral("//"))
      {
        Start += 2;
      }
    }
    qsizetype PSlash = Url.indexOf(QLatin1Char('/'), Start);
    if (PSlash < 0)
    {
      PSlash = Url.size();
    }
    QString ConnectInfo = Url.mid(Start, PSlash - Start);
    qsizetype At = ConnectInfo.lastIndexOf(QLatin1Char('@'));
    if (At < 0)
    {
      return Url;
    }
    QStringList UserInfo = ConnectInfo.left(At).split(QLatin1Char(';'));
    QStringList Kept;
    for (int Index = 0; Index < UserInfo.size(); Index++)
    {
      if ((Index == 0) || !UserInfo[Index].startsWith(QStringLiteral("x-"), Qt::CaseInsensitive))
      {
        Kept.append(UserInfo[Index]);
      }
    }
    return Url.left(Start) + Kept.join(QLatin1Char(';')) + ConnectInfo.mid(At) + Url.mid(PSlash);
  }

  // Settings TSessionData::DoLoad reads only when !Unsafe
  void RestoreUnsafeSettings(SessionData & Data, const SessionData & Base)
  {
    Data.Shell = Base.Shell;
    Data.ListingCommand = Base.ListingCommand;
    Data.DeleteToRecycleBin = Base.DeleteToRecycleBin;
    Data.OverwrittenToRecycleBin = Base.OverwrittenToRecycleBin;
    Data.RecycleBinPath = Base.RecycleBinPath;
    Data.PostLoginCommands = Base.PostLoginCommands;
    Data.ReturnVar = Base.ReturnVar;
    Data.ProxyLocalCommand = Base.ProxyLocalCommand;
    Data.ProxyTelnetCommand = Base.ProxyTelnetCommand;
    Data.SftpServer = Base.SftpServer;
  }

  QString DefaultKeyFileValue;

  QString CliExecutable()
  {
    // the folder of this program (Contents/MacOS in the macOS app bundle)
    QString Self = QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath();
    if (!Self.isEmpty())
    {
      QString Candidate = QFileInfo(Self).absolutePath() + QStringLiteral("/nixscp-cli");
      if (QFileInfo(Candidate).isExecutable())
      {
        return Candidate;
      }
    }
    return QStringLiteral("nixscp-cli");
  }

  QString NormalizeRemoteDir(QString Dir)
  {
    while ((Dir.size() > 1) && Dir.endsWith(QLatin1Char('/')))
    {
      Dir.chop(1);
    }
    return Dir;
  }

  // UnixIsChildPath (or the same directory)
  bool IsSameOrChildPath(const QString & Parent, const QString & Child)
  {
    QString P = NormalizeRemoteDir(Parent);
    QString C = NormalizeRemoteDir(Child);
    return (P == C) || (P == QStringLiteral("/")) || C.startsWith(P + QLatin1Char('/'));
  }

  // TSessionData::IsSameSite (TTerminalManager::IsAvailableTerminalForSite)
  bool IsSameSite(const SessionData & A, const SessionData & B)
  {
    bool SameProtocol = (A.FSProtocol == B.FSProtocol) || (A.IsSsh() && B.IsSsh());
    return SameProtocol &&
      (A.HostName.compare(B.HostName, Qt::CaseInsensitive) == 0) &&
      (A.PortNumber == B.PortNumber) &&
      (A.UserName == B.UserName);
  }

  QList<FilePanel *> VisibleRemotePanels(CommanderWindow * Window)
  {
    QList<FilePanel *> Result;
    for (FilePanel * Panel : Window->findChildren<FilePanel *>())
    {
      if (Panel->IsRemote() && Panel->isVisible())
      {
        Result.append(Panel);
      }
    }
    return Result;
  }
}

bool GuiCommandLine::CheckSafe(Options & Params)
{
  // windows/UserInterface.cpp CheckSafe: silent, as warning would help the attacker
  return !Params.FindSwitch(QStringLiteral("Unsafe"));
}

GuiCommandLine::ParsedUrl GuiCommandLine::ParseUrl(const QString & Url, Options * Params, StoredSessionList & Stored, bool Unsafe)
{
  ParsedUrl Result;
  QString Scheme;
  TFSProtocol Protocol = fsSFTP;
  int ProtocolLen = 0;
  bool ProtocolDefined = CommandLineUrlProtocol(Url, Scheme, Protocol, ProtocolLen);
  QString Rest = Url.mid(ProtocolLen);
  if (ProtocolDefined && Rest.startsWith(QStringLiteral("//")))
  {
    Rest.remove(0, 2);
  }
  // pufAllowStoredSiteWithProtocol: "sftp://site/path" uses the stored site
  // (lets a site named by the host provide e.g. the default user name)
  std::shared_ptr<SessionData> Site = !Rest.isEmpty() ? MatchStoredSite(Stored, DecodeUrlChars(Rest)) : nullptr;
  QString UrlToParse = (Site && ProtocolDefined) ? Rest : Url;

  SessionExtras Extras;
  ParseSessionUrl(UrlToParse, Params, &Stored, Result.Data, Extras, Result.ParsedInfo);
  if (Site && ProtocolDefined)
  {
    Result.Data.FSProtocol = Protocol;
    Result.ParsedInfo |= piProtocolDefined;
  }
  Result.Passphrase = Extras.Passphrase;
  Result.PuttyProtocol = ProtocolDefined && (Scheme == QStringLiteral("ssh"));

  // Remote directory part and the session parameters (";save")
  QString RawRemote;
  QString SessionParams;
  if (Site)
  {
    qsizetype P = 1;
    while ((P < Rest.size()) && (DecodeUrlChars(Rest.left(P)).compare(Site->Name, Qt::CaseInsensitive) != 0))
    {
      P++;
    }
    RawRemote = Rest.mid(P);
  }
  else if (!Rest.isEmpty())
  {
    qsizetype PSlash = Rest.indexOf(QLatin1Char('/'));
    if (PSlash >= 0)
    {
      QString WithParams = Rest.mid(PSlash);
      qsizetype PParam = WithParams.indexOf(QLatin1Char(';'));
      RawRemote = (PParam >= 0) ? WithParams.left(PParam) : WithParams;
      SessionParams = (PParam >= 0) ? WithParams.mid(PParam + 1) : QString();
    }
  }
  for (const QString & SessionParam : SessionParams.split(QLatin1Char(';'), Qt::SkipEmptyParts))
  {
    qsizetype P = SessionParam.indexOf(QLatin1Char('='));
    QString Name = (P >= 0) ? SessionParam.left(P) : SessionParam;
    if (Name.compare(QStringLiteral("save"), Qt::CaseInsensitive) == 0)
    {
      bool Ok = false;
      int Value = (P >= 0) ? SessionParam.mid(P + 1).toInt(&Ok) : 1;
      Result.SaveOnly = ((P >= 0) && !Ok) ? true : (Value != 0);
    }
  }
  // A path not ending with a slash points to a file (to be downloaded)
  if (!RawRemote.isEmpty() && (RawRemote != QStringLiteral("/")) && !RawRemote.endsWith(QLatin1Char('/')))
  {
    qsizetype Slash = RawRemote.lastIndexOf(QLatin1Char('/'));
    Result.DownloadFile = DecodeUrlChars(RawRemote.mid(Slash + 1));
    Result.Data.RemoteDirectory = DecodeUrlChars(RawRemote.left(Slash + 1));
  }

  if (Unsafe)
  {
    // pufUnsafe: what the URL or /rawsettings would set is reverted to the
    // site's (or default) values
    SessionData Base;
    SessionExtras BaseExtras;
    int BaseInfo = 0;
    ParseSessionUrl(StripUrlRawSettings(UrlToParse), nullptr, &Stored, Base, BaseExtras, BaseInfo);
    RestoreUnsafeSettings(Result.Data, Base);
  }
  return Result;
}

QString GuiCommandLine::FolderOrWorkspaceName(const QString & SessionName, Options * Params, bool PreferSite,
  const StoredSessionList & Stored)
{
  QString Name = DecodeUrlChars(SessionName);
  if ((Params == nullptr) || !Params->FindSwitch(QStringLiteral("workspace")))
  {
    PreferSite = true;
  }
  if (Name.isEmpty())
  {
    return QString();
  }
  if ((!PreferSite || (Stored.FindByName(Name) == nullptr)) &&
      (Workspaces::IsFolder(Stored, Name) || Workspaces::IsWorkspace(Stored, Name)))
  {
    return Name;
  }
  return QString();
}

bool GuiCommandLine::DoLoginDialog(QWidget * Parent, const SessionData * Initial, bool ShowAgain, LoginData & Result)
{
  Configuration & Config = Configuration::Instance();
  LoginDialog Dialog(Config.StoredSessions(), Parent);
  if (Initial != nullptr)
  {
    Dialog.SetInitialSession(*Initial);
  }
  Dialog.SetShowAgainPanel(ShowAgain);
  if (Dialog.exec() != QDialog::Accepted)
  {
    return false;
  }
  Result.Sessions = Dialog.SessionsToOpen();
  if (Result.Sessions.isEmpty() && Dialog.SessionToOpen())
  {
    Result.Sessions.append(*Dialog.SessionToOpen());
  }
  QString Name = Dialog.WorkspaceToOpen();
  if (!Name.isEmpty() && Workspaces::IsWorkspace(Config.StoredSessions(), Name))
  {
    Result.Workspace = Name;
  }
  return !Result.Sessions.isEmpty() || !Result.Workspace.isEmpty();
}

GuiCommandLine::LoginData GuiCommandLine::GetLoginData(const QString & SessionName, Options & Params, LoginNeed Need,
  QWidget * LinkedForm, bool Unsafe)
{
  Configuration & Config = Configuration::Instance();
  StoredSessionList & Stored = Config.StoredSessions();
  LoginData Result;
  bool DefaultsOnly = false;
  std::shared_ptr<SessionData> LastStoredSession;

  bool PreferSession = (Need == lnTerminal);
  QString FolderOrWorkspace = FolderOrWorkspaceName(SessionName, &Params, PreferSession, Stored);
  if (!FolderOrWorkspace.isEmpty())
  {
    if (Workspaces::IsWorkspace(Stored, FolderOrWorkspace))
    {
      Result.Workspace = FolderOrWorkspace;
    }
    Result.Sessions = Workspaces::FolderOrWorkspace(Stored, FolderOrWorkspace);
  }
  else
  {
    ParsedUrl Parsed = ParseUrl(SessionName, &Params, Stored, Unsafe);
    DefaultsOnly = (Parsed.ParsedInfo & piDefaultsOnly) != 0;
    Result.DownloadFile = Parsed.DownloadFile;
    if (Parsed.SaveOnly)
    {
      // ";save": Save session dialog, then the Login dialog with the new site
      std::shared_ptr<SessionData> Saved = DoSaveSession(Parsed.Data, LinkedForm);
      if (Saved == nullptr)
      {
        Result.Aborted = true;
        return Result;
      }
      LastStoredSession = Saved;
      Result.DownloadFile.clear();
    }
    else if (Parsed.PuttyProtocol)
    {
      // ssh:// opens a terminal session, not a file transfer one
      OpenSessionInTerminal(Parsed.Data);
      Result.Aborted = true;
      return Result;
    }
    else
    {
      Result.Sessions.append(Parsed.Data);
    }
  }

  if (DefaultsOnly && (Need == lnNone))
  {
    // No URL and no operation needing a session: noop
    Result.Sessions.clear();
  }
  else if (Result.Workspace.isEmpty() &&
           (Result.Sessions.isEmpty() || !Stored.CanOpen(Result.Sessions.first()) || DefaultsOnly))
  {
    // A session is needed for the operation, the ";save" URL was handled,
    // or the session cannot be opened (no host name)
    SessionData Initial;
    const SessionData * InitialPtr = nullptr;
    if (LastStoredSession)
    {
      Initial = *LastStoredSession;
      InitialPtr = &Initial;
    }
    else if (!Result.Sessions.isEmpty() && !DefaultsOnly)
    {
      Initial = Result.Sessions.first();
      InitialPtr = &Initial;
    }
    LoginData Dialog;
    if (!DoLoginDialog(LinkedForm, InitialPtr, false, Dialog))
    {
      Result.Sessions.clear();
      Result.Aborted = true;
      return Result;
    }
    Result.Sessions = Dialog.Sessions;
    Result.Workspace = Dialog.Workspace;
  }
  return Result;
}

std::shared_ptr<SessionData> GuiCommandLine::DoSaveSession(const SessionData & Data, QWidget * Parent)
{
  // TSaveSessionDialog (forms/Custom.cpp); "Create desktop shortcut" is not offered
  Configuration & Config = Configuration::Instance();
  StoredSessionList & Stored = Config.StoredSessions();

  QDialog Dialog(Parent);
  Dialog.setWindowTitle(QStringLiteral("Save session as site"));
  Dialog.setObjectName(QStringLiteral("SaveSessionDialog"));
  auto * Layout = new QVBoxLayout(&Dialog);
  auto * NameLabel = new QLabel(QStringLiteral("&Site name:"));
  auto * NameEdit = new QLineEdit();
  NameEdit->setObjectName(QStringLiteral("SessionNameEdit"));
  NameLabel->setBuddy(NameEdit);
  Layout->addWidget(NameLabel);
  Layout->addWidget(NameEdit);
  auto * FolderLabel = new QLabel(QStringLiteral("&Folder:"));
  auto * FolderCombo = new QComboBox();
  FolderCombo->setObjectName(QStringLiteral("FolderCombo"));
  FolderCombo->setEditable(true);
  FolderLabel->setBuddy(FolderCombo);
  Layout->addWidget(FolderLabel);
  Layout->addWidget(FolderCombo);
  const QString RootFolder = QStringLiteral("<none>");
  QStringList Folders;
  for (const auto & Site : Stored.Sessions())
  {
    QString Folder = Site->FolderName();
    if (!Site->IsWorkspace && !Folder.isEmpty() && !Folders.contains(Folder, Qt::CaseInsensitive))
    {
      Folders.append(Folder);
    }
  }
  Folders.sort(Qt::CaseInsensitive);
  FolderCombo->addItem(RootFolder);
  FolderCombo->addItems(Folders);
  bool CanSavePassword = Data.HasAnySessionPassword() && !Config.DisablePasswordStoring();
  auto * SavePasswordCheck = new QCheckBox(Config.UseMasterPassword() ?
    QStringLiteral("Save &password (protected by master password)") : QStringLiteral("Save &password (not recommended)"));
  SavePasswordCheck->setObjectName(QStringLiteral("SavePasswordCheck"));
  SavePasswordCheck->setEnabled(CanSavePassword);
  Layout->addWidget(SavePasswordCheck);
  auto * Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  // TCustomDialog(HELP_SESSION_SAVE)
  HelpSystem::AddHelpButton(Buttons, &Dialog, QStringLiteral("ui_login_save"));
  Layout->addWidget(Buttons);

  QString SessionName = Data.Name.isEmpty() ? Data.DefaultSessionName() : Data.Name;
  NameEdit->setText(SessionData::MakeValidName(Data.Name.isEmpty() ? SessionName : Data.LocalName()));
  QString InitialFolder = Data.Name.isEmpty() ? QString() : Data.FolderName();
  FolderCombo->setCurrentText(InitialFolder.isEmpty() ? RootFolder : InitialFolder);
  NameEdit->selectAll();

  auto ComposedName = [&]()
  {
    QString Folder = FolderCombo->currentText().trimmed();
    if (Folder == RootFolder)
    {
      Folder.clear();
    }
    return SessionData::ComposePath(Folder, NameEdit->text().trimmed());
  };
  QObject::connect(Buttons, &QDialogButtonBox::rejected, &Dialog, &QDialog::reject);
  QObject::connect(Buttons, &QDialogButtonBox::accepted, &Dialog, [&]()
  {
    // SessionNameValidate
    QString Error;
    QString Name = ComposedName();
    if (NameEdit->text().trimmed().isEmpty() || !SessionData::IsValidName(NameEdit->text().trimmed(), &Error))
    {
      ShowErrorMessage(&Dialog, Error.isEmpty() ? QStringLiteral("Site name cannot be empty.") : Error);
      return;
    }
    if ((Stored.FindByName(Name) != nullptr) &&
        (MessageDialog(&Dialog, QStringLiteral("Site with name '%1' already exists. Overwrite?").arg(Name),
          qtConfirmation, qaOK | qaCancel) != qaOK))
    {
      return;
    }
    Dialog.accept();
  });
  if (Dialog.exec() != QDialog::Accepted)
  {
    return nullptr;
  }
  SessionData ToSave = Data;
  ToSave.IsWorkspace = false;
  if (!(CanSavePassword && SavePasswordCheck->isChecked()))
  {
    ToSave.Password.clear();
    ToSave.TunnelPassword.clear();
  }
  std::shared_ptr<SessionData> Saved = Stored.NewSession(ComposedName(), ToSave);
  if (!Config.Save())
  {
    ShowErrorMessage(Parent, Config.LastSaveError());
  }
  return Saved;
}

void GuiCommandLine::SetDefaultKeyFile(const QString & FileName)
{
  DefaultKeyFileValue = FileName;
}

QString GuiCommandLine::DefaultKeyFile()
{
  return DefaultKeyFileValue;
}

SessionData GuiCommandLine::PrepareSession(const SessionData & Data)
{
  SessionData Result = Data;
  Result.IsWorkspace = false;
  // TSessionData::ResolvePublicKeyFile: the /privatekey default
  if (Result.PublicKeyFile.isEmpty() && Result.IsSsh() && !DefaultKeyFileValue.isEmpty())
  {
    Result.PublicKeyFile = DefaultKeyFileValue;
  }
  Result.ExpandEnvironmentVariables();
  return Result;
}

bool GuiCommandLine::OpenSessionInTerminal(const SessionData & Data)
{
  // windows/GUITools.cpp OpenSessionInPutty (ssh:// URLs, /putty): the same
  // terminal client command as Commands > Open in PuTTY (all patterns, the
  // OpenSSH options of the session, terminal emulator fallback, password
  // passed as SSH_ASKPASS with "Remember session password and pass it to PuTTY")
  return DesktopIntegration::OpenSessionInTerminal(nullptr, Data, Data.Password);
}

//---------------------------------------------------------------------------
// Configuration switches

void GuiCommandLine::ApplyIniSwitch(Options & Params, QString * IniFileMissing)
{
  // windows/UserInterface.cpp CreateConfiguration
  Configuration & Config = Configuration::Instance();
  QString IniFileName = Params.SwitchValue(QStringLiteral("ini"), QString());
  if (IniFileName.isEmpty())
  {
    return;
  }
  if (IniFileName.compare(QStringLiteral("nul"), Qt::CaseInsensitive) == 0)
  {
    Config.SetIniFileName(QStringLiteral("nul"));
  }
  else if (CheckSafe(Params))
  {
    QString Expanded = QFileInfo(SessionData::ExpandEnvironmentVariables(IniFileName)).absoluteFilePath();
    if (!QFileInfo::exists(Expanded) && (IniFileMissing != nullptr))
    {
      *IniFileMissing = Expanded;
    }
    Config.SetIniFileName(Expanded);
  }
}

void GuiCommandLine::ApplyRawConfig(Options & Params)
{
  // TConfiguration::OptionsStorage: settings layered over the configuration.
  // The configuration of this run is a temporary copy with the settings applied.
  QStringList RawConfig;
  if (!CheckSafe(Params) || !Params.FindSwitch(QStringLiteral("rawconfig"), RawConfig) || RawConfig.isEmpty())
  {
    return;
  }
  static QTemporaryDir Temp;
  if (!Temp.isValid())
  {
    return;
  }
  Configuration & Config = Configuration::Instance();
  QString FileName = Temp.filePath(QStringLiteral("nixscp.ini"));
  if (!Config.Export(FileName))
  {
    return;
  }
  IniFile File;
  File.Load(FileName);
  for (const QString & Setting : RawConfig)
  {
    qsizetype P = Setting.indexOf(QLatin1Char('='));
    QString Path = (P >= 0) ? Setting.left(P) : Setting;
    QString Value = (P >= 0) ? Setting.mid(P + 1) : QString();
    qsizetype Slash = Path.lastIndexOf(QLatin1Char('\\'));
    QString KeyPath = QStringLiteral("Configuration") + ((Slash >= 0) ? (QLatin1Char('\\') + Path.left(Slash)) : QString());
    QString Name = (Slash >= 0) ? Path.mid(Slash + 1) : Path;
    IniStorage Storage(&File);
    if (Storage.OpenSubKeyPath(KeyPath, true))
    {
      Storage.WriteString(Name, Value);
      Storage.CloseSubKeyPath();
    }
  }
  File.Save(FileName);
  Config.SetIniFileName(FileName);
  Config.Load();
}

void GuiCommandLine::ApplyLoggingAndTransferSwitches(Options & Params)
{
  Configuration & Config = Configuration::Instance();
  QString SwitchValue;
  // the switches apply to this run only (TConfiguration::TemporaryLog*)
  SessionLog::BeginTemporaryLogging(Config);
  auto EndTemporary = qScopeGuard([&Config]() { SessionLog::EndTemporaryLogging(Config); });
  // /loglevel=<-1..2>[*[-]] (TemporaryLogProtocol / TemporaryLogSensitive)
  if (Params.FindSwitch(QStringLiteral("loglevel"), SwitchValue))
  {
    qsizetype StarPos = SwitchValue.indexOf(QLatin1Char('*'));
    if (StarPos >= 0)
    {
      bool LogSensitive = true;
      SwitchValue.remove(StarPos, 1);
      if ((StarPos < SwitchValue.size()) && (SwitchValue[StarPos] == QLatin1Char('-')))
      {
        LogSensitive = false;
        SwitchValue.remove(StarPos, 1);
      }
      SwitchValue = SwitchValue.trimmed();
      Config.LogSensitive = LogSensitive;
    }
    bool Ok = false;
    int LogProtocol = SwitchValue.toInt(&Ok);
    if (!SwitchValue.isEmpty() && Ok && (LogProtocol >= -1))
    {
      Config.LogProtocol = LogProtocol;
    }
  }
  // /logsize=[<count>*]<size> (TemporaryLogMaxCount / TemporaryLogMaxSize)
  if (Params.FindSwitch(QStringLiteral("logsize"), SwitchValue))
  {
    qsizetype StarPos = SwitchValue.indexOf(QLatin1Char('*'));
    int LogMaxCount = 0;
    if (StarPos > 0)
    {
      bool Ok = false;
      LogMaxCount = SwitchValue.left(StarPos).toInt(&Ok);
      if (!Ok)
      {
        LogMaxCount = -1;
      }
      SwitchValue = SwitchValue.mid(StarPos + 1).trimmed();
    }
    qint64 LogMaxSize = 0;
    if ((LogMaxCount >= 0) && !SwitchValue.isEmpty() && TryStrToSize(SwitchValue, LogMaxSize))
    {
      Config.LogMaxCount = LogMaxCount;
      Config.LogMaxSize = LogMaxSize;
    }
  }
  // /rawtransfersettings setting1=value1 ... (LoadDefaultCopyParam from TOptionsStorage)
  QStringList RawTransferSettings;
  if (Params.FindSwitch(QStringLiteral("rawtransfersettings"), RawTransferSettings) && !RawTransferSettings.isEmpty())
  {
    IniFile File;
    IniStorage Storage(&File);
    Storage.OpenSubKey(QStringLiteral("CopyParam"), true);
    for (const QString & Setting : RawTransferSettings)
    {
      qsizetype P = Setting.indexOf(QLatin1Char('='));
      Storage.WriteString((P >= 0) ? Setting.left(P) : Setting, (P >= 0) ? Setting.mid(P + 1) : QString());
    }
    Config.DefaultCopyParam().Load(Storage);
  }
}

void GuiCommandLine::ApplyLogSwitches(Options & Params)
{
  if (!CheckSafe(Params))
  {
    return;
  }
  Configuration & Config = Configuration::Instance();
  SessionLog::BeginTemporaryLogging(Config);
  auto EndTemporary = qScopeGuard([&Config]() { SessionLog::EndTemporaryLogging(Config); });
  QString LogFile;
  // TConfiguration::TemporaryLogging: a .xml file is the XML log
  if (Params.FindSwitch(QStringLiteral("log"), LogFile) && !LogFile.isEmpty())
  {
    if (QFileInfo(LogFile).suffix().compare(QStringLiteral("xml"), Qt::CaseInsensitive) == 0)
    {
      Config.LogActions = true;
      Config.ActionsLogFileName = LogFile;
    }
    else
    {
      Config.Logging = true;
      Config.LogFileName = LogFile;
    }
  }
  // TConfiguration::TemporaryActionsLogging
  if (Params.FindSwitch(QStringLiteral("XmlLog"), LogFile) && !LogFile.isEmpty())
  {
    Config.LogActions = true;
    Config.ActionsLogFileName = LogFile;
  }
}

int GuiCommandLine::RunConsoleMode(const QStringList & Arguments)
{
  Options Params;
  for (const QString & Arg : Arguments)
  {
    Params.Add(Arg);
  }
  bool Scripting = false;
  bool Console =
    Arguments.contains(QStringLiteral("--help")) ||
    Params.FindSwitch(QStringLiteral("help")) || Params.FindSwitch(QStringLiteral("h")) || Params.FindSwitch(QStringLiteral("?")) ||
    Params.FindSwitch(QStringLiteral("batchsettings")) || Params.FindSwitch(QStringLiteral("keygen")) ||
    Params.FindSwitch(QStringLiteral("copyid")) || Params.FindSwitch(QStringLiteral("fingerprintscan")) ||
    Params.FindSwitch(QStringLiteral("info"));
  if (!Console)
  {
    QString Value;
    // "/console=<ver>" is the wrapper version (BACKWARD COMPATIBILITY), not the mode
    bool ConsoleSwitch = Params.FindSwitch(QStringLiteral("console"), Value) && Value.isEmpty();
    Scripting = ConsoleSwitch || Params.FindSwitch(QStringLiteral("script")) || Params.FindSwitch(QStringLiteral("command"));
    Console = Scripting;
  }
  if (!Console)
  {
    return -1;
  }
  QString Cli = CliExecutable();
  QStringList Args;
  // Started from the desktop without a terminal: the console needs one
  bool NeedTerminal = Scripting && !isatty(STDIN_FILENO) && !isatty(STDOUT_FILENO);
  if (NeedTerminal)
  {
    // x-terminal-emulator, else $TERMINAL / xdg-terminal-exec / the desktop's terminal
    QStringList EmulatorArgs{QStringLiteral("-e")};
    QString Emulator = DesktopIntegration::ResolveTerminalProgram(QStringLiteral("x-terminal-emulator"), EmulatorArgs);
    Args << Emulator << EmulatorArgs << Cli;
  }
  else
  {
    Args << Cli;
  }
  for (const QString & Arg : Arguments)
  {
    // "/console" only switches the binary into the console mode
    if ((Arg.compare(QStringLiteral("/console"), Qt::CaseInsensitive) != 0) &&
        (Arg.compare(QStringLiteral("-console"), Qt::CaseInsensitive) != 0))
    {
      Args << Arg;
    }
  }
  std::vector<QByteArray> Storage;
  std::vector<char *> Argv;
  for (const QString & Arg : Args)
  {
    Storage.push_back(Arg.toLocal8Bit());
  }
  for (QByteArray & A : Storage)
  {
    Argv.push_back(A.data());
  }
  Argv.push_back(nullptr);
  fflush(stdout);
  execvp(Argv[0], Argv.data());
  fprintf(stderr, "Cannot run %s.\n", Argv[0]);
  return 1;
}

//---------------------------------------------------------------------------
// Sessions in the main window

void GuiCommandLine::OpenSessions(CommanderWindow * Window, const LoginData & Data)
{
  if (!Data.Workspace.isEmpty())
  {
    Window->OpenWorkspace(Data.Workspace);
    return;
  }
  for (const SessionData & Session : Data.Sessions)
  {
    Window->OpenSession(PrepareSession(Session));
  }
}

void GuiCommandLine::ExploreFile(CommanderWindow * Window, const QString & FileName)
{
  // TCustomScpExplorerForm::ExploreFile: focus the file once the remote panel
  // of the (connecting) session lists its directory
  QString Name = UnixPath::ExtractFileName(FileName);
  QString Dir = FileName.contains(QLatin1Char('/')) ? UnixPath::ExtractFileDir(FileName) : QString();
  if (Name.isEmpty())
  {
    return;
  }
  QPointer<CommanderWindow> Guard(Window);
  auto * Timer = new QTimer(Window);
  auto Elapsed = std::make_shared<QElapsedTimer>();
  Elapsed->start();
  Timer->setInterval(200);
  QObject::connect(Timer, &QTimer::timeout, Timer, [Guard, Timer, Name, Dir, Elapsed]()
  {
    if (!Guard || (Elapsed->elapsed() > 60000))
    {
      Timer->deleteLater();
      return;
    }
    for (FilePanel * Panel : VisibleRemotePanels(Guard))
    {
      if ((Dir.isEmpty() || (NormalizeRemoteDir(Panel->CurrentDirectory()) == NormalizeRemoteDir(Dir))) &&
          Panel->FocusFile(Name))
      {
        Timer->deleteLater();
        return;
      }
    }
  });
  Timer->start();
}

bool GuiCommandLine::CanCommandLineFromAnotherInstance(CommanderWindow * Window)
{
  // Visible && !NonVisualDataModule->Busy (no modal dialog or menu open)
  return (Window != nullptr) && Window->isVisible() &&
    (QApplication::activeModalWidget() == nullptr) && (QApplication::activePopupWidget() == nullptr);
}

void GuiCommandLine::CommandLineFromAnotherInstance(CommanderWindow * Window, const QStringList & Arguments,
  const QString & WorkingDirectory, const QString & ActivationToken)
{
  Options Params;
  for (const QString & Arg : Arguments)
  {
    Params.Add(Arg);
  }
  if ((Window == nullptr) || (Params.ParamCount() == 0))
  {
    return;
  }
  // Initiated by another process: bring the window to the front
  if (!ActivationToken.isEmpty() && (QGuiApplication::platformName() == QStringLiteral("wayland")))
  {
    qputenv("XDG_ACTIVATION_TOKEN", ActivationToken.toUtf8());
  }
  if (Window->isMinimized())
  {
    Window->showNormal();
  }
  Window->raise();
  Window->activateWindow();
  // Focus stealing prevention (X11 has no use for the sender's token after
  // startup; Wayland compositors may refuse a stale one): at least ask for
  // attention (taskbar flash / "NixSCP is ready" notification)
  QApplication::alert(Window);

  // The session may have been stored by the other instance just now
  Configuration & Config = Configuration::Instance();
  Config.Reload();

  QString OldDirectory = QDir::currentPath();
  if (!WorkingDirectory.isEmpty())
  {
    QDir::setCurrent(WorkingDirectory);
  }
  try
  {
    LoginData Data = GetLoginData(Params.Param(1), Params, lnSession, Window, !CheckSafe(Params));
    QDir::setCurrent(OldDirectory);
    if (!Data.Aborted)
    {
      OpenSessions(Window, Data);
    }
  }
  catch (const ExtException & E)
  {
    QDir::setCurrent(OldDirectory);
    ShowErrorMessage(Window, E.Message(), E.MoreMessages());
  }
}

void GuiCommandLine::RefreshPanel(CommanderWindow * Window, const QString & Session, const QString & Path)
{
  // TCustomScpExplorerForm::RefreshPanel: reload the remote panel showing
  // the session (any when not given) in Path (or under it; any when not given)
  if (Window == nullptr)
  {
    return;
  }
  SessionData Filter;
  bool HasFilter = !Session.isEmpty();
  if (HasFilter)
  {
    try
    {
      Filter = ParseUrl(Session, nullptr, Configuration::Instance().StoredSessions(), true).Data;
    }
    catch (const ExtException &)
    {
      return;
    }
  }
  QList<Terminal *> Terminals = Window->findChildren<Terminal *>(Qt::FindDirectChildrenOnly);
  for (FilePanel * Panel : VisibleRemotePanels(Window))
  {
    QString Directory = Panel->CurrentDirectory();
    if (Directory.isEmpty() || (!Path.isEmpty() && !IsSameOrChildPath(Path, Directory)))
    {
      continue;
    }
    bool Match = !HasFilter;
    for (Terminal * Term : Terminals)
    {
      if (Match)
      {
        break;
      }
      Match = Term->Active() && IsSameSite(Term->Data(), Filter);
    }
    if (Match)
    {
      Panel->Refresh();
    }
  }
}

//---------------------------------------------------------------------------
// URL handler registration (windows/Setup.cpp)

QStringList GuiCommandLine::UrlSchemes()
{
  return {
    QStringLiteral("sftp"), QStringLiteral("scp"), QStringLiteral("ftp"), QStringLiteral("ftps"),
    QStringLiteral("ftpes"), QStringLiteral("dav"), QStringLiteral("davs"), QStringLiteral("s3") };
}

QString GuiCommandLine::UrlHandlerDesktopFileName()
{
  return QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation) +
    QStringLiteral("/nixscp-url-handler.desktop");
}

#ifdef Q_OS_WIN
// Windows: as WinSCP (RegisterProtocol), per user: HKCU\Software\Classes\<scheme>
static const QString ClassesKey = QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes");

static QString UrlHandlerCommand()
{
  // /unsafe, so that a link cannot use dangerous switches (CheckSafe)
  return QStringLiteral("\"%1\" /unsafe \"%2\"")
    .arg(QDir::toNativeSeparators(QCoreApplication::applicationFilePath()), QStringLiteral("%1"));
}
#endif

bool GuiCommandLine::RegisterForDefaultProtocols(QString * Error)
{
#ifdef Q_OS_MACOS
  // needs CFBundleURLTypes and the open URL event; not implemented yet
  if (Error != nullptr)
  {
    *Error = QStringLiteral("Handling URL addresses is not available on macOS yet.");
  }
  return false;
#endif
#ifdef Q_OS_WIN
  QSettings Classes(ClassesKey, QSettings::NativeFormat);
  for (const QString & Scheme : UrlSchemes())
  {
    Classes.beginGroup(Scheme);
    Classes.setValue(QStringLiteral("Default"), QStringLiteral("URL:%1 Protocol").arg(Scheme.toUpper()));
    Classes.setValue(QStringLiteral("URL Protocol"), QString());
    Classes.setValue(QStringLiteral("EditFlags"), 2);
    Classes.setValue(QStringLiteral("DefaultIcon/Default"),
      QDir::toNativeSeparators(QCoreApplication::applicationFilePath()) + QStringLiteral(",0"));
    Classes.setValue(QStringLiteral("shell/open/command/Default"), UrlHandlerCommand());
    Classes.endGroup();
  }
  Classes.sync();
  if (Classes.status() != QSettings::NoError)
  {
    if (Error != nullptr)
    {
      *Error = QStringLiteral("Cannot register NixSCP for the URL protocols in the registry.");
    }
    return false;
  }
  return true;
#endif
  QString FileName = UrlHandlerDesktopFileName();
  QString Dir = QFileInfo(FileName).absolutePath();
  QStringList MimeTypes;
  for (const QString & Scheme : UrlSchemes())
  {
    MimeTypes.append(QStringLiteral("x-scheme-handler/") + Scheme);
  }
  QDir().mkpath(Dir);
  QFile File(FileName);
  if (!File.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
  {
    if (Error != nullptr)
    {
      *Error = QStringLiteral("Cannot create file \"%1\".").arg(FileName);
    }
    return false;
  }
  // As WinSCP's handler: /unsafe, so that a link cannot use dangerous switches (CheckSafe)
  QString Exec = QCoreApplication::applicationFilePath();
  File.write(QStringLiteral("[Desktop Entry]\nType=Application\nName=NixSCP\nComment=Open file transfer URLs in NixSCP\n"
                            "Exec=\"%1\" /unsafe %u\nIcon=nixscp\nTerminal=false\nNoDisplay=true\nMimeType=%2;\n")
               .arg(Exec, MimeTypes.join(QLatin1Char(';'))).toUtf8());
  File.close();
  if (!QStandardPaths::findExecutable(QStringLiteral("xdg-mime")).isEmpty())
  {
    QProcess::execute(QStringLiteral("xdg-mime"), QStringList{QStringLiteral("default"), QFileInfo(FileName).fileName()} + MimeTypes);
  }
  if (!QStandardPaths::findExecutable(QStringLiteral("update-desktop-database")).isEmpty())
  {
    QProcess::execute(QStringLiteral("update-desktop-database"), {Dir});
  }
  return true;
}

bool GuiCommandLine::UnregisterForProtocols(QString * Error)
{
#ifdef Q_OS_MACOS
  if (Error != nullptr)
  {
    *Error = QStringLiteral("Handling URL addresses is not available on macOS yet.");
  }
  return false;
#endif
#ifdef Q_OS_WIN
  // only the schemes that are still ours
  QSettings Classes(ClassesKey, QSettings::NativeFormat);
  for (const QString & Scheme : UrlSchemes())
  {
    if (Classes.value(Scheme + QStringLiteral("/shell/open/command/Default")).toString() == UrlHandlerCommand())
    {
      Classes.remove(Scheme);
    }
  }
  Classes.sync();
  if (Classes.status() != QSettings::NoError)
  {
    if (Error != nullptr)
    {
      *Error = QStringLiteral("Cannot unregister NixSCP for the URL protocols in the registry.");
    }
    return false;
  }
  return true;
#endif
  QString FileName = UrlHandlerDesktopFileName();
  if (QFile::exists(FileName))
  {
    if (!QFile::remove(FileName))
    {
      if (Error != nullptr)
      {
        *Error = QStringLiteral("Cannot delete file \"%1\".").arg(FileName);
      }
      return false;
    }
    if (!QStandardPaths::findExecutable(QStringLiteral("update-desktop-database")).isEmpty())
    {
      QProcess::execute(QStringLiteral("update-desktop-database"), {QFileInfo(FileName).absolutePath()});
    }
  }
  return true;
}

QString GuiCommandLine::PersonalFolder()
{
  QString Result = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
  if (Result.isEmpty() || !QFileInfo(Result).isDir())
  {
    Result = QDir::homePath();
  }
  return Result;
}

//---------------------------------------------------------------------------
// CommandLineOperation (Upload / Download / FullSynchronize / Synchronize of WinMain.cpp)

CommandLineOperation::CommandLineOperation(Kind AKind, const SessionData & Data, const QStringList & Params,
  int UseDefaults, QObject * Parent) :
  QObject(Parent),
  FKind(AKind),
  FData(Data),
  FParams(Params),
  FUseDefaults(UseDefaults)
{
}

CommandLineOperation::~CommandLineOperation()
{
  delete FTerm;
  FTerm = nullptr;
  delete FUI;
}

void CommandLineOperation::Start()
{
  SessionData Data = GuiCommandLine::PrepareSession(FData);
  FUI = new GuiUserInterface(nullptr);
  FTerm = new Terminal(Data, FUI, this);
  FTerm->SetSettings(Terminal::ConfigurationSettings());
  auto Cancelled = std::make_shared<bool>(false);
  QPointer<CommandLineOperation> Guard(this);
  FUI->BeginConnecting(Data, [Guard, Cancelled]()
  {
    *Cancelled = true;
    if (Guard && Guard->FTerm)
    {
      Guard->FTerm->Close();
    }
  });
  FTerm->Open([Guard, Cancelled](const OperationResult & Result)
  {
    if (!Guard)
    {
      return;
    }
    Guard->FUI->EndConnecting();
    if (!Result.Ok || *Cancelled)
    {
      bool Error = !Result.Aborted && !*Cancelled;
      if (Error)
      {
        // Connecting to the auto-start session failed: no Login dialog
        ShowErrorMessage(nullptr, Result.Message, Result.MoreMessages);
      }
      Guard->Finish(Error ? 1 : 0);
      return;
    }
    Guard->Connected();
  });
}

void CommandLineOperation::Connected()
{
  switch (FKind)
  {
    case okUpload:
      Upload();
      break;
    case okDownload:
      Download();
      break;
    case okFullSynchronize:
      Synchronize(false);
      break;
    case okKeepUpToDate:
      Synchronize(true);
      break;
  }
}

int CommandLineOperation::ExecCopyDialog(CopyDialog & Dialog)
{
  if (FUseDefaults > 0)
  {
    // /defaults=n: the dialog submits itself after n seconds
    QPointer<CopyDialog> Guard(&Dialog);
    QTimer::singleShot(FUseDefaults * 1000, &Dialog, [Guard]()
    {
      if (Guard && Guard->isVisible())
      {
        Guard->accept();
      }
    });
  }
  return Dialog.exec();
}

void CommandLineOperation::Transfer(bool ToRemote, const QStringList & Paths, const QStringList & DisplayNames,
  const QString & TargetDirectory)
{
  CopyParam Param = Configuration::Instance().DefaultCopyParam();
  QString Target = TargetDirectory;
  if (FUseDefaults != 0)
  {
    CopyDialog Dialog(ToRemote, false, DisplayNames, TargetDirectory, Param, nullptr);
    Dialog.SetOptions(CopyDialog::coDisableQueue);
    Dialog.SetCopyParamAttrs(CopyParam::UsableAttrs(ToRemote, FTerm));
    if (ExecCopyDialog(Dialog) != QDialog::Accepted)
    {
      Finish(0);
      return;
    }
    Target = Dialog.TargetDirectory();
    Param = Dialog.Param();
  }
  Param.Queue = false;
  auto * Progress = new ProgressDialog(FTerm->Progress(), nullptr);
  Progress->Start();
  QPointer<CommandLineOperation> Guard(this);
  auto Done = [Guard, Progress](const OperationResult & Result)
  {
    Progress->Finish(Result.Ok);
    Progress->deleteLater();
    if (!Guard)
    {
      return;
    }
    if (!Result.Ok && !Result.Aborted)
    {
      ShowErrorMessage(nullptr, Result.Message, Result.MoreMessages);
    }
    // CopyParam.OnceDoneOperation = odoDisconnect
    Guard->Finish((Result.Ok || Result.Aborted) ? 0 : 1);
  };
  if (ToRemote)
  {
    FTerm->CopyToRemote(Paths, Target, Param, false, Done);
  }
  else
  {
    FTerm->CopyToLocal(Paths, Target, Param, false, Done);
  }
}

void CommandLineOperation::Upload()
{
  QStringList Paths;
  QStringList Names;
  for (const QString & Param : FParams)
  {
    QFileInfo Info(SessionData::ExpandEnvironmentVariables(Param));
    QString Path = Info.absoluteFilePath();
    while ((Path.size() > 1) && Path.endsWith(QLatin1Char('/')))
    {
      Path.chop(1);
    }
    Paths.append(Path);
    Names.append(QFileInfo(Path).fileName());
  }
  Transfer(true, Paths, Names, FTerm->CurrentDirectory());
}

void CommandLineOperation::Download()
{
  QString Path = FParams.value(0);
  QString Directory = UnixPath::ExtractFileDir(Path);
  QString Name = UnixPath::ExtractFileName(Path);
  QPointer<CommandLineOperation> Guard(this);
  // Terminal->ReadFile (ExceptionOnFail)
  FTerm->ReadDirectory(Directory, [Guard, Path, Name](const OperationResult & Result, const QString &, const RemoteFileList & Files)
  {
    if (!Guard)
    {
      return;
    }
    if (!Result.Ok)
    {
      if (!Result.Aborted)
      {
        ShowErrorMessage(nullptr, Result.Message, Result.MoreMessages);
      }
      Guard->Finish(Result.Aborted ? 0 : 1);
      return;
    }
    bool Found = false;
    for (const RemoteFile & File : Files)
    {
      if (File.FileName == Name)
      {
        Found = true;
        break;
      }
    }
    if (!Found)
    {
      // FILE_NOT_EXISTS
      ShowErrorMessage(nullptr, QStringLiteral("File or folder '%1' does not exist.").arg(Path));
      Guard->Finish(1);
      return;
    }
    QString LocalDirectory = Guard->FData.LocalDirectoryExpanded();
    if (LocalDirectory.isEmpty())
    {
      LocalDirectory = GuiCommandLine::PersonalFolder();
    }
    Guard->Transfer(false, QStringList{Path}, QStringList{Name}, LocalDirectory);
  });
}

void CommandLineOperation::Synchronize(bool KeepUpToDate)
{
  // SynchronizeDirectories: [local directory [remote directory]]
  QString LocalDirectory;
  if (FParams.size() >= 1)
  {
    LocalDirectory = QFileInfo(SessionData::ExpandEnvironmentVariables(FParams[0])).absoluteFilePath();
  }
  else if (!FData.LocalDirectory.isEmpty())
  {
    LocalDirectory = FData.LocalDirectoryExpanded();
  }
  else
  {
    LocalDirectory = GuiCommandLine::PersonalFolder();
  }
  QString RemoteDirectory = (FParams.size() >= 2) ? FParams[1] : FTerm->CurrentDirectory();
  QPointer<CommandLineOperation> Guard(this);
  auto Finished = [Guard]()
  {
    if (Guard)
    {
      Guard->Finish(0);
    }
  };
  CopyParam Param = Configuration::Instance().DefaultCopyParam();
  if (KeepUpToDate)
  {
    RunKeepRemoteUpToDate(nullptr, FTerm, LocalDirectory, RemoteDirectory, Param, QStringList(), Finished);
  }
  else
  {
    RunFullSynchronize(nullptr, FTerm, LocalDirectory, RemoteDirectory, true, Param, QStringList(), Finished);
  }
}

void CommandLineOperation::Finish(int ExitCode)
{
  if (FDone)
  {
    return;
  }
  FDone = true;
  if (FTerm != nullptr)
  {
    FTerm->Close();
  }
  if (Finished)
  {
    Finished(ExitCode);
  }
}

//---------------------------------------------------------------------------
// LastSessionWatcher (LastTerminalClosed -> NeedSession)

LastSessionWatcher::LastSessionWatcher(CommanderWindow * Window) :
  QObject(Window),
  FWindow(Window)
{
  FTimer = new QTimer(this);
  FTimer->setSingleShot(true);
  FTimer->setInterval(400);
  connect(FTimer, &QTimer::timeout, this, &LastSessionWatcher::Check);
  // Sessions (Terminal) and the Login dialog are children of the window
  Window->installEventFilter(this);
  FLastCount = SessionCount();
}

void LastSessionWatcher::SetInterval(int Ms)
{
  FTimer->setInterval(Ms);
}

bool LastSessionWatcher::eventFilter(QObject * Object, QEvent * Event)
{
  if ((Object == FWindow) && ((Event->type() == QEvent::ChildAdded) || (Event->type() == QEvent::ChildRemoved)))
  {
    // the child may not be constructed (or already destructed) yet
    if (!FTimer->isActive())
    {
      QTimer::singleShot(0, this, &LastSessionWatcher::Check);
    }
  }
  return false;
}

int LastSessionWatcher::SessionCount() const
{
  // Sessions open or being opened; a failed or closed one may still exist
  // until its deferred deletion (e.g. after the Login dialog it caused)
  int Result = 0;
  if (FWindow)
  {
    for (Terminal * Term : FWindow->findChildren<Terminal *>(Qt::FindDirectChildrenOnly))
    {
      if (Term->Active() || Term->Busy())
      {
        Result++;
      }
    }
  }
  return Result;
}

void LastSessionWatcher::Check()
{
  if (!FWindow || FWindow->property("HandlesLastSessionClosed").toBool() || QCoreApplication::closingDown())
  {
    return;
  }
  // A Login dialog (e.g. after a failed connect) takes care of the session need
  if (!FWindow->findChildren<LoginDialog *>(Qt::FindDirectChildrenOnly).isEmpty())
  {
    FLastCount = 0;
    FPending = false;
    return;
  }
  int Count = SessionCount();
  if (Count > 0)
  {
    FLastCount = Count;
    FPending = false;
    return;
  }
  if (FLastCount > 0)
  {
    FLastCount = 0;
    FPending = true;
  }
  if (!FPending)
  {
    return;
  }
  if (!FWindow->isVisible())
  {
    FPending = false;
    return;
  }
  if ((QApplication::activeModalWidget() != nullptr) || (QApplication::activePopupWidget() != nullptr))
  {
    // e.g. the "connection lost" message: decide once it is closed
    FTimer->start();
    return;
  }
  FPending = false;
  if (Configuration::Instance().ShowLoginOnStartup)
  {
    GuiCommandLine::LoginData Data;
    if (GuiCommandLine::DoLoginDialog(FWindow, nullptr, true, Data) && FWindow)
    {
      GuiCommandLine::OpenSessions(FWindow, Data);
    }
  }
}
