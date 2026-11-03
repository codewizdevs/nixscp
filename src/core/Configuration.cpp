// Port of WinSCP source/core/Configuration.cpp, windows/GUIConfiguration.cpp,
// windows/CustomWinConfiguration.cpp and windows/WinConfiguration.cpp (subset),
// storage handling of core/HierarchicalStorage.cpp (TIniFileStorage::Flush:
// re-read and merge before writing, so several instances can share the file)
#include "core/Configuration.h"
#include "core/ConfigVault.h"
#include "core/CloudSettings.h"
#include "core/FileMasks.h"
#include "core/Security.h"
#include "core/SessionLog.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QLockFile>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>

#include <cmath>
#include <cstring>

static const QString InterfaceKey = QStringLiteral("Interface");
static const QString ConfigurationKey = QStringLiteral("Configuration");
static const QString SecurityKey = QStringLiteral("Configuration\\Security");
static const QString SessionsSectionPrefix = QStringLiteral("Sessions\\");
static const double SecsPerDay = 24 * 60 * 60;

//---------------------------------------------------------------------------
// THierarchicalStorage::ReadDateTime/WriteDateTime (TDateTime as hex of double)
static double ReadDateTime(IniStorage & Storage, const QString & Name, double Default)
{
  QString Value = Storage.ReadStringRaw(Name, QString());
  if (Value.isEmpty())
  {
    return Default;
  }
  QByteArray Raw = QByteArray::fromHex(Value.toLatin1());
  double Result;
  if (Raw.size() == static_cast<int>(sizeof(Result)))
  {
    memcpy(&Result, Raw.constData(), sizeof(Result));
    return Result;
  }
  bool Ok;
  Result = Value.toDouble(&Ok);
  return Ok ? Result : Default;
}

static void WriteDateTime(IniStorage & Storage, const QString & Name, double Value)
{
  Storage.WriteStringRaw(Name, QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(&Value), sizeof(Value)).toHex().toUpper()));
}

static int ReadSeconds(IniStorage & Storage, const QString & Name, int Default)
{
  return static_cast<int>(std::lround(ReadDateTime(Storage, Name, Default / SecsPerDay) * SecsPerDay));
}

static void WriteSeconds(IniStorage & Storage, const QString & Name, int Seconds)
{
  WriteDateTime(Storage, Name, Seconds / SecsPerDay);
}

// The configuration file, decrypted when it is encrypted (NixSCP Pro);
// Locked: encrypted and not unlocked (then empty and never to be overwritten)
static QByteArray ReadFileData(const QString & FileName, bool * Locked = nullptr)
{
  return ConfigVault::ReadFile(FileName, Locked);
}

static QString ExpandPath(const QString & Path)
{
  // ExpandEnvironmentVariables counterpart: ~ and $VAR / ${VAR}
  QString Result = Path.trimmed();
  if ((Result == QStringLiteral("~")) || Result.startsWith(QStringLiteral("~/")))
  {
    Result = QDir::homePath() + Result.mid(1);
  }
  static const QRegularExpression VarRegExp(QStringLiteral("\\$(\\{([A-Za-z_][A-Za-z0-9_]*)\\}|([A-Za-z_][A-Za-z0-9_]*))"));
  QRegularExpressionMatch Match;
  int From = 0;
  while ((Match = VarRegExp.match(Result, From)).hasMatch())
  {
    QString Name = !Match.captured(2).isEmpty() ? Match.captured(2) : Match.captured(3);
    QString Value = qEnvironmentVariable(Name.toLocal8Bit().constData());
    Result.replace(Match.capturedStart(), Match.capturedLength(), Value);
    From = Match.capturedStart() + Value.size();
  }
  return Result;
}

// Stored (encrypted) password values of sites; the same password encrypted
// twice differs because of random padding (or salt), see Configuration::Save
static bool IsSessionPasswordKey(const QString & Key)
{
  static const QStringList Keys = {
    QStringLiteral("Password"), QStringLiteral("ProxyPasswordEnc"), QStringLiteral("TunnelPassword"),
    QStringLiteral("TunnelPassphrase"), QStringLiteral("Passphrase"), QStringLiteral("EncryptKey"),
    QStringLiteral("NewPassword") };
  return Keys.contains(Key, Qt::CaseInsensitive);
}

static bool PasswordEquivalent(const QString & Section, const QString & Key, const QString & From, const QString & To)
{
  return
    Section.startsWith(SessionsSectionPrefix, Qt::CaseInsensitive) && IsSessionPasswordKey(Key) &&
    Security::SamePassword(From.toLatin1(), To.toLatin1());
}

// Policy DisablePasswordStoring: no passwords in the sites (TSessionData::SavePasswords)
static void StripSessionPasswords(IniFile & File)
{
  static const QStringList Keys = {
    QStringLiteral("Password"), QStringLiteral("PasswordPlain"), QStringLiteral("ProxyPasswordEnc"),
    QStringLiteral("ProxyPassword"), QStringLiteral("TunnelPassword"), QStringLiteral("TunnelPasswordPlain"),
    QStringLiteral("TunnelPassphrase"), QStringLiteral("TunnelPassphrasePlain"), QStringLiteral("Passphrase"),
    QStringLiteral("PassphrasePlain"), QStringLiteral("EncryptKey"), QStringLiteral("EncryptKeyPlain") };
  for (const QString & Section : File.Sections())
  {
    if (Section.startsWith(SessionsSectionPrefix, Qt::CaseInsensitive))
    {
      for (const QString & Key : Keys)
      {
        File.DeleteKey(Section, Key);
      }
    }
  }
}

// The user's own configuration (without the machine-wide defaults, which are
// part of the working copy): the user's file with this instance's unsaved
// changes merged in, as Save would write it
static IniFile UserConfiguration(const QByteArray & DiskData, const IniFile & Baseline, const IniFile & Current)
{
  IniFile Result;
  Result.LoadFromData(DiskData);
  Result.Apply(IniFile::Diff(Baseline, Current, PasswordEquivalent));
  Result.EraseSection(QString());
  return Result;
}

//---------------------------------------------------------------------------
QString EditorPreferences::Name() const
{
  // TEditorPreferences::GetName
  switch (Editor)
  {
    case edInternal:
      return QStringLiteral("Internal editor");
    case edOpen:
      return QStringLiteral("Associated application");
    default:
    {
      QString Command = ExternalEditor.trimmed();
      QString Program;
      if (Command.startsWith(QLatin1Char('"')))
      {
        int End = Command.indexOf(QLatin1Char('"'), 1);
        Program = (End > 0) ? Command.mid(1, End - 1) : Command.mid(1);
      }
      else
      {
        Program = Command.section(QLatin1Char(' '), 0, 0);
      }
      return QFileInfo(Program).fileName();
    }
  }
}

//---------------------------------------------------------------------------
ConfigurationData::ConfigurationData()
{
  LogFileName = Configuration::DefaultLogFileName();
  ActionsLogFileName = SessionLog::DefaultActionsLogFileName();
  PuttyPath = Configuration::DefaultPuttyPath();
  Editors = Configuration::DefaultEditors();
}

//---------------------------------------------------------------------------
Configuration & Configuration::Instance()
{
  static Configuration Instance;
  return Instance;
}

// ~/.config/nixscp on Linux; %APPDATA%\NixSCP (roaming, as WinSCP.ini) on
// Windows; ~/Library/Application Support/NixSCP on macOS
static QString ConfigurationDirectory()
{
#ifdef Q_OS_WIN
  QString AppData = qEnvironmentVariable("APPDATA");
  if (!AppData.isEmpty())
  {
    return QDir::fromNativeSeparators(AppData) + QStringLiteral("/NixSCP");
  }
#elif defined(Q_OS_MACOS)
  return QDir::homePath() + QStringLiteral("/Library/Application Support/NixSCP");
#endif
  return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/nixscp");
}

Configuration::Configuration()
{
  QString Dir = ConfigurationDirectory();
#ifdef Q_OS_LINUX
  // One-time migration from the project's former name (LinSCP): settings,
  // sites and the override file move over on the first start of NixSCP
  QString OldDir = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/linscp");
  if (!QFileInfo::exists(Dir) && QFileInfo(OldDir).isDir())
  {
    QDir().mkpath(Dir);
    QFile::copy(OldDir + QStringLiteral("/linscp.ini"), Dir + QStringLiteral("/nixscp.ini"));
    QFile::copy(OldDir + QStringLiteral("/override.ini"), Dir + QStringLiteral("/override.ini"));
    QFile::setPermissions(Dir + QStringLiteral("/nixscp.ini"), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  }
#endif
  QDir().mkpath(Dir);
  // TConfiguration::LoadCustomIniFileStorageName ("WinSCP 2 Override" key)
  IniFile Override;
  if (Override.Load(OverrideFileName()))
  {
    IniStorage Storage(&Override);
    if (Storage.OpenSubKey(QStringLiteral("Override"), false))
    {
      FCustomIniFileName = Storage.ReadString(QStringLiteral("IniFile"), QString());
    }
  }
  FIniFileName = !FCustomIniFileName.isEmpty() ? ExpandPath(FCustomIniFileName) : AutomaticIniFileName();
}


QString Configuration::OverrideFileName()
{
  return ConfigurationDirectory() + QStringLiteral("/override.ini");
}

QString Configuration::PortableIniFileName()
{
  // TConfiguration::GetAutomaticIniFileStorageName: an INI file next to the
  // executable wins (here also next to the AppImage file)
  QStringList Candidates;
  QString AppImage = qEnvironmentVariable("APPIMAGE");
  if (!AppImage.isEmpty())
  {
    Candidates.append(QFileInfo(AppImage).absolutePath() + QStringLiteral("/nixscp.ini"));
  }
  if (QCoreApplication::instance() != nullptr)
  {
    Candidates.append(QCoreApplication::applicationDirPath() + QStringLiteral("/nixscp.ini"));
  }
  for (const QString & Candidate : Candidates)
  {
    if (QFileInfo(Candidate).isFile())
    {
      return Candidate;
    }
  }
  return QString();
}

QString Configuration::AutomaticIniFileName()
{
  QString Portable = PortableIniFileName();
  if (!Portable.isEmpty())
  {
    return Portable;
  }
  return ConfigurationDirectory() + QStringLiteral("/nixscp.ini");
}

void Configuration::SetIniFileName(const QString & FileName)
{
  // TConfiguration::SetExplicitIniFileStorageName; "nul" = stNul
  FExplicitIniFileName = FileName;
  FNulStorage = (FileName.compare(QStringLiteral("nul"), Qt::CaseInsensitive) == 0);
  FIniFileName = FileName;
  FDiskData.clear();
  // not loaded from this file (yet)
  FLoaded = false;
  if (FWatcher != nullptr)
  {
    FWatcher->removePaths(FWatcher->files() + FWatcher->directories());
  }
}

void Configuration::SaveOverride()
{
  // TConfiguration::SaveCustomIniFileStorageName
  QString FileName = OverrideFileName();
  if (FCustomIniFileName.isEmpty())
  {
    QFile::remove(FileName);
  }
  else
  {
    IniFile Override;
    IniStorage Storage(&Override);
    Storage.OpenSubKey(QStringLiteral("Override"), true);
    Storage.WriteString(QStringLiteral("IniFile"), FCustomIniFileName);
    QDir().mkpath(QFileInfo(FileName).absolutePath());
    Override.Save(FileName);
  }
}

QStringList Configuration::SystemIniFileNames() const
{
  if (FSystemIniFileNamesSet)
  {
    return FSystemIniFileNames;
  }
  // $XDG_CONFIG_DIRS (most important first), without the user's own directory
  QStringList Result;
  QString UserDir = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
  QString Own = QFileInfo(FIniFileName).absoluteFilePath();
  for (const QString & Dir : QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation))
  {
    if (Dir == UserDir)
    {
      continue;
    }
    QString FileName = Dir + QStringLiteral("/nixscp/nixscp.ini");
    if (QFileInfo(FileName).isFile() && (QFileInfo(FileName).absoluteFilePath() != Own))
    {
      Result.append(FileName);
    }
  }
  return Result;
}

void Configuration::SetSystemIniFileNames(const QStringList & FileNames)
{
  FSystemIniFileNames = FileNames;
  FSystemIniFileNamesSet = true;
}

QString Configuration::DefaultLogFileName()
{
  // TConfiguration::GetDefaultLogFileName: %TEMP%\!S.log; on Linux in
  // $XDG_STATE_HOME/nixscp (logs can contain sensitive data, /tmp is world readable)
  return SessionLog::DefaultLogFileName();
}

QString Configuration::DefaultPuttyPath()
{
  // Linux replacement for PuTTY: OpenSSH client in the default terminal
  return QStringLiteral("x-terminal-emulator -e ssh -p !# !U@!@");
}

QList<EditorPreferences> Configuration::DefaultEditors()
{
  // TWinConfiguration::LoadFrom adds the internal editor when the list is empty
  EditorPreferences Internal;
  return { Internal };
}

const EditorPreferences * Configuration::EditorPreferencesFor(const QString & FileName) const
{
  // TEditorList::Find
  QString Name = FileName;
  int Slash = Name.lastIndexOf(QLatin1Char('/'));
  if (Slash >= 0)
  {
    Name = Name.mid(Slash + 1);
  }
  for (const EditorPreferences & Editor : Editors)
  {
    try
    {
      FileMasks Masks(Editor.FileMask);
      if (Masks.Matches(Name, false, false))
      {
        return &Editor;
      }
    }
    catch (...)
    {
      // invalid mask never matches
    }
  }
  return nullptr;
}

QString Configuration::EditorFor(const QString & FileName) const
{
  const EditorPreferences * Editor = EditorPreferencesFor(FileName);
  if (Editor == nullptr)
  {
    return QString();
  }
  switch (Editor->Editor)
  {
    case edExternal:
      return Editor->ExternalEditor.trimmed().isEmpty() ? QString() : Editor->ExternalEditor.trimmed();
    case edOpen:
#ifdef Q_OS_MACOS
      return QStringLiteral("open !.!");
#else
      return QStringLiteral("xdg-open !.!");
#endif
    default:
      return QString();
  }
}

QString Configuration::FormatEditorCommand(const QString & Command, const QString & LocalFileName)
{
  // ExternalEditor: "!.!" (or "%1") is replaced with the file name,
  // otherwise the file name is appended.
  // The result is safe for a POSIX shell: the name is double-quoted only when
  // it has no character the shell expands inside double quotes, otherwise it
  // is single-quoted (' as '\''). Editors are started without a shell anyway
  // (gui/EditorManager::EditorCommandArguments); this is the display form.
  static const QString ShellSpecial = QStringLiteral("\"$`\\!");
  bool NeedsSingle = false;
  for (QChar C : LocalFileName)
  {
    NeedsSingle = NeedsSingle || ShellSpecial.contains(C) || (C.unicode() < 0x20);
  }
  QString Quoted = NeedsSingle ?
    (QLatin1Char('\'') + QString(LocalFileName).replace(QLatin1Char('\''), QStringLiteral("'\\''")) + QLatin1Char('\'')) :
    (QLatin1Char('"') + LocalFileName + QLatin1Char('"'));
  // one left-to-right pass, so the inserted name is never scanned again
  auto Expand = [&Command, &Quoted](const QString & Pattern)
  {
    QString Result;
    const QString Patterns[] = { QLatin1Char('"') + Pattern + QLatin1Char('"'), QLatin1Char('\'') + Pattern + QLatin1Char('\''), Pattern };
    int Index = 0;
    while (Index < Command.size())
    {
      bool Matched = false;
      for (const QString & P : Patterns)
      {
        if (QStringView(Command).mid(Index).startsWith(P))
        {
          Result += Quoted;
          Index += static_cast<int>(P.size());
          Matched = true;
          break;
        }
      }
      if (!Matched)
      {
        Result += Command.at(Index++);
      }
    }
    return Result;
  };
  QString Result = Command;
  if (Command.contains(QStringLiteral("!.!")))
  {
    Result = Expand(QStringLiteral("!.!"));
  }
  else if (Command.contains(QStringLiteral("%1")))
  {
    Result = Expand(QStringLiteral("%1"));
  }
  else
  {
    Result += QLatin1Char(' ') + Quoted;
  }
  return Result;
}

//---------------------------------------------------------------------------
void Configuration::Default()
{
  static_cast<ConfigurationData &>(*this) = ConfigurationData();
  FCopyParam = CopyParam();
}

void Configuration::LoadFrom(IniFile & File, bool Prompt)
{
  IniStorage Storage(&File);

  // Configuration\Security first, the site passwords may need the master password
  bool UseMasterPassword = false;
  QString MasterPasswordVerifier;
  if (Storage.OpenSubKeyPath(SecurityKey, false))
  {
    UseMasterPassword = Storage.ReadBool(QStringLiteral("UseMasterPassword"), false);
    MasterPasswordVerifier = Storage.ReadString(QStringLiteral("MasterPasswordVerifier"), QString());
    Storage.CloseSubKeyPath();
  }
  Security::SetMasterPasswordState(UseMasterPassword && !MasterPasswordVerifier.isEmpty(), MasterPasswordVerifier);
  {
    // ask for the master password once at most
    Security::MasterPasswordSession Session;
    std::unique_ptr<Security::NoMasterPasswordPrompt> NoPrompt(Prompt ? nullptr : new Security::NoMasterPasswordPrompt());
    FStoredSessions.Load(File);
  }
  if (FDisablePasswordStoring)
  {
    // stored passwords are not used (and stay untouched in the file)
    QList<SessionData *> All;
    All.append(&FStoredSessions.DefaultSettings());
    for (const auto & Data : FStoredSessions.Sessions())
    {
      All.append(Data.get());
    }
    for (SessionData * Data : All)
    {
      Data->Password.clear();
      Data->ProxyPassword.clear();
      Data->TunnelPassword.clear();
    }
  }

  if (Storage.OpenSubKey(ConfigurationKey, false))
  {
    if (Storage.OpenSubKey(InterfaceKey, false))
    {
      #define READ_BOOL(VAR, NAME) VAR = Storage.ReadBool(QStringLiteral(NAME), VAR)
      #define READ_INT(VAR, NAME) VAR = Storage.ReadInteger(QStringLiteral(NAME), VAR)
      #define READ_ENUM(VAR, NAME) VAR = static_cast<decltype(VAR)>(Storage.ReadInteger(QStringLiteral(NAME), VAR))
      #define READ_STRING(VAR, NAME) VAR = Storage.ReadString(QStringLiteral(NAME), VAR)

      // TConfiguration
      READ_BOOL(FCopyParam.ConfirmOverwriting, "ConfirmOverwriting");
      READ_BOOL(FCopyParam.ConfirmResume, "ConfirmResume");
      READ_BOOL(AutoReadDirectoryAfterOp, "AutoReadDirectoryAfterOp");
      READ_INT(SessionReopenAuto, "SessionReopenAuto");
      READ_INT(SessionReopenTimeout, "SessionReopenTimeout");
      READ_INT(SessionReopenAutoStall, "SessionReopenAutoStall");
      READ_STRING(ExternalIpAddress, "ExternalIpAddress");
      READ_INT(LocalPortNumberMin, "LocalPortNumberMin");
      READ_INT(LocalPortNumberMax, "LocalPortNumberMax");
      READ_BOOL(TryFtpWhenSshFails, "TryFtpWhenSshFails");
      READ_INT(QueueTransfersLimit, "QueueTransfersLimit");
      READ_INT(ParallelTransferThreshold, "ParallelTransferThreshold");
      // TGUIConfiguration
      READ_BOOL(ContinueOnError, "ContinueOnError");
      READ_BOOL(ConfirmCommandSession, "ConfirmCommandSession");
      READ_BOOL(QueueKeepDoneItems, "QueueKeepDoneItems");
      READ_INT(QueueKeepDoneItemsFor, "QueueKeepDoneItemsFor");
      READ_BOOL(QueueAutoPopup, "QueueAutoPopup");
      READ_BOOL(SessionRememberPassword, "QueueRememberPassword");
      READ_STRING(PuttyPath, "PuttyPath");
      READ_BOOL(PuttyPassword, "PuttyPassword");
      READ_BOOL(TelnetForFtpInPutty, "TelnetForFtpInPutty");
      READ_BOOL(BeepOnFinish, "BeepOnFinish");
      BeepOnFinishAfter = ReadSeconds(Storage, QStringLiteral("BeepOnFinishAfter"), BeepOnFinishAfter);
      READ_INT(SessionReopenAutoIdle, "SessionReopenAutoIdle");
      READ_BOOL(SessionReopenAutoIdleOn, "SessionReopenAutoIdleOn");
      READ_BOOL(SessionReopenAutoInactive, "SessionReopenAutoInactive");
      READ_BOOL(SessionSilentDisconnect, "SessionSilentDisconnect");
      // TCustomWinConfiguration
      READ_BOOL(ConfirmExitOnCompletion, "ConfirmExitOnCompletion");
      READ_BOOL(SynchronizeSummary, "SynchronizeSummary");
      // TWinConfiguration
      READ_ENUM(DoubleClickAction, "CopyOnDoubleClick");
      READ_BOOL(CopyOnDoubleClickConfirmation, "CopyOnDoubleClickConfirmation");
      READ_BOOL(AlwaysRespectDoubleClickAction, "AlwaysRespectDoubleClickAction");
      READ_ENUM(DDTransferConfirmation, "DDTransferConfirmation2");
      READ_STRING(DDTemporaryDirectory, "DDTemporaryDirectory");
      READ_BOOL(DDWarnLackOfTempSpace, "DDWarnLackOfTempSpace");
      READ_BOOL(DeleteToRecycleBin, "DeleteToRecycleBin");
      READ_BOOL(RenameWholeName, "RenameWholeName");
      READ_BOOL(ShowHiddenFiles, "ShowHiddenFiles");
      READ_ENUM(FormatSizeBytes, "FormatSizeBytes");
      READ_ENUM(PanelSearch, "PanelSearch");
      READ_BOOL(ShowInaccesibleDirectories, "ShowInaccesibleDirectories");
      READ_BOOL(ConfirmTransferring, "ConfirmTransferring");
      READ_BOOL(ConfirmDeleting, "ConfirmDeleting");
      READ_BOOL(ConfirmRecycling, "ConfirmRecycling");
      READ_BOOL(ConfirmClosingSession, "ConfirmClosingSession");
      READ_BOOL(UseLocationProfiles, "UseLocationProfiles");
      READ_INT(Locale, "LocaleSafe");
      READ_BOOL(DefaultDirIsHome, "DefaultDirIsHome");
      READ_BOOL(TemporaryDirectoryAppendSession, "TemporaryDirectoryAppendSession");
      READ_BOOL(TemporaryDirectoryAppendPath, "TemporaryDirectoryAppendPath");
      READ_BOOL(TemporaryDirectoryDeterministic, "TemporaryDirectoryDeterministic");
      READ_BOOL(TemporaryDirectoryCleanup, "TemporaryDirectoryCleanup");
      READ_BOOL(ConfirmTemporaryDirectoryCleanup, "ConfirmTemporaryDirectoryCleanup");
      READ_BOOL(PreservePanelState, "PreservePanelState");
      READ_ENUM(DarkTheme, "DarkTheme");
      READ_BOOL(AutoSaveWorkspace, "AutoSaveWorkspace");
      READ_BOOL(AutoSaveWorkspacePasswords, "AutoSaveWorkspacePasswords");
      READ_STRING(AutoWorkspace, "AutoWorkspace");
      READ_BOOL(WorkspaceConnectAll, "WorkspaceConnectAll");
      READ_ENUM(PathInCaption, "PathInCaption");
      READ_ENUM(SessionTabNameFormat, "SessionTabNameFormat");
      READ_BOOL(MinimizeToTray, "MinimizeToTray");
      READ_BOOL(BalloonNotifications, "BalloonNotifications");
      READ_BOOL(CopyParamAutoSelectNotice, "CopyParamAutoSelectNotice");
      READ_BOOL(AutoOpenInPutty, "AutoOpenInPutty");
      READ_BOOL(RefreshRemotePanel, "RefreshRemotePanel");
      RefreshRemotePanelInterval = ReadSeconds(Storage, QStringLiteral("RefreshRemotePanelInterval"), RefreshRemotePanelInterval);
      READ_STRING(PanelFontName, "PanelFontName");
      READ_INT(PanelFontSize, "PanelFontSize");
      READ_BOOL(NaturalOrderNumericalSorting, "NaturalOrderNumericalSorting");
      READ_BOOL(AlwaysSortDirectoriesByName, "AlwaysSortDirectoriesByName");
      READ_BOOL(FullRowSelect, "FullRowSelect");
      READ_BOOL(EnableQueueByDefault, "EnableQueueByDefault");
      READ_BOOL(ExternalSessionInExistingInstance, "ExternalSessionInExistingInstance");
      READ_BOOL(ShowLoginOnStartup, "ShowLoginWhenNoSession");
      READ_BOOL(KeepOpenWhenNoSession, "KeepOpenWhenNoSession");
      READ_BOOL(FlashTaskbar, "FlashTaskbar");
      READ_BOOL(EditorCheckNotModified, "EditorCheckNotModified");
      READ_BOOL(SessionTabCaptionTruncation, "SessionTabCaptionTruncation");
      CopyOnDoubleClick = (DoubleClickAction == dcaCopy);

      // TGUIConfiguration::LoadDefaultCopyParam: Configuration\Interface\CopyParam
      if (Storage.OpenSubKey(QStringLiteral("CopyParam"), false))
      {
        FCopyParam.Load(Storage);
        Storage.CloseSubKey();
      }
      if (Storage.OpenSubKey(QStringLiteral("Editor"), false))
      {
        READ_STRING(EditorFontName, "FontName2");
        READ_INT(EditorFontSize, "FontSize");
        READ_BOOL(EditorWordWrap, "WordWrap");
        READ_INT(EditorTabSize, "TabSize");
        READ_BOOL(EditorWarnOrLargeFileSize, "WarnOrLargeFileSize");
        READ_INT(EditorLargeFileSize, "LargeFileSize");
        READ_INT(EditorEncoding, "Encoding");
        READ_INT(EditorFontColor, "FontColor");
        READ_INT(EditorBackgroundColor, "BackgroundColor");
        // TEditorList::Load
        QList<EditorPreferences> List;
        for (int Index = 0; Storage.OpenSubKey(QString::number(Index), false); Index++)
        {
          EditorPreferences E;
          READ_STRING(E.FileMask, "FileMask");
          READ_ENUM(E.Editor, "Editor");
          READ_STRING(E.ExternalEditor, "ExternalEditor");
          READ_BOOL(E.ExternalEditorText, "ExternalEditorText");
          READ_BOOL(E.SDIExternalEditor, "SDIExternalEditor");
          READ_BOOL(E.DetectMDIExternalEditor, "DetectMDIExternalEditor");
          List.append(E);
          Storage.CloseSubKey();
        }
        if (!List.isEmpty())
        {
          Editors = List;
        }
        Storage.CloseSubKey();
      }
      if (Storage.OpenSubKey(QStringLiteral("QueueView"), false))
      {
        READ_ENUM(QueueViewShow, "Show");
        Storage.CloseSubKey();
      }
      if (Storage.OpenSubKey(QStringLiteral("Commander"), false))
      {
        READ_BOOL(SwappedPanels, "SwappedPanels");
        READ_ENUM(NortonLikeMode, "ExplorerStyleSelection");
        READ_BOOL(PreserveLocalDirectory, "PreserveLocalDirectory");
        READ_BOOL(CompareByTime, "CompareByTime");
        READ_BOOL(CompareBySize, "CompareBySize");
        READ_BOOL(TreeOnLeft, "TreeOnLeft");
        READ_BOOL(ExplorerKeyboardShortcuts, "ExplorerKeyboardShortcuts");
        READ_BOOL(SystemContextMenu, "SystemContextMenu");
        Storage.CloseSubKey();
      }
      if (Storage.OpenSubKey(QStringLiteral("NixSCP"), false))
      {
        WindowGeometry = QByteArray::fromHex(Storage.ReadStringRaw(QStringLiteral("WindowGeometry"), QString()).toLatin1());
        WindowState = QByteArray::fromHex(Storage.ReadStringRaw(QStringLiteral("WindowState"), QString()).toLatin1());
        PanelSplitter = QByteArray::fromHex(Storage.ReadStringRaw(QStringLiteral("PanelSplitter"), QString()).toLatin1());
        LastLocalDirectory = Storage.ReadString(QStringLiteral("LastLocalDirectory"), LastLocalDirectory);
        CheckForUpdates = Storage.ReadBool(QStringLiteral("CheckForUpdates"), CheckForUpdates);
        UpdateLastCheck = QDateTime::fromString(Storage.ReadString(QStringLiteral("UpdateLastCheck"), QString()), Qt::ISODate);
        UpdateSkipVersion = Storage.ReadString(QStringLiteral("UpdateSkipVersion"), UpdateSkipVersion);
        Storage.CloseSubKey();
      }
      Storage.CloseSubKey();
    }
    if (Storage.OpenSubKey(QStringLiteral("Logging"), false))
    {
      READ_BOOL(Logging, "Logging");
      READ_STRING(LogFileName, "LogFileName");
      READ_BOOL(LogFileAppend, "LogFileAppend");
      READ_BOOL(LogSensitive, "LogSensitive");
      LogMaxSize = Storage.ReadStringRaw(QStringLiteral("LogMaxSize"), QString::number(LogMaxSize)).toLongLong();
      READ_INT(LogMaxCount, "LogMaxCount");
      READ_INT(LogProtocol, "LogProtocol");
      READ_BOOL(LogActions, "LogActions");
      READ_STRING(ActionsLogFileName, "ActionsLogFileName");
      Storage.CloseSubKey();
    }
    #undef READ_BOOL
    #undef READ_INT
    #undef READ_ENUM
    #undef READ_STRING
    Storage.CloseSubKey();
  }
}

void Configuration::SaveTo(IniFile & File)
{
  FStoredSessions.Save(File);
  if (FDisablePasswordStoring)
  {
    StripSessionPasswords(File);
  }

  // Legacy flag set by GUI code that does not know DoubleClickAction
  if (CopyOnDoubleClick && (DoubleClickAction != dcaCopy))
  {
    DoubleClickAction = dcaCopy;
  }
  else if (!CopyOnDoubleClick && (DoubleClickAction == dcaCopy))
  {
    DoubleClickAction = dcaOpen;
  }

  IniStorage Storage(&File);
  if (Storage.OpenSubKey(ConfigurationKey, true))
  {
    #define WRITE_BOOL(VAR, NAME) Storage.WriteBool(QStringLiteral(NAME), VAR)
    #define WRITE_INT(VAR, NAME) Storage.WriteInteger(QStringLiteral(NAME), VAR)
    #define WRITE_STRING(VAR, NAME) Storage.WriteString(QStringLiteral(NAME), VAR)
    if (Storage.OpenSubKey(InterfaceKey, true))
    {
      WRITE_BOOL(FCopyParam.ConfirmOverwriting, "ConfirmOverwriting");
      WRITE_BOOL(FCopyParam.ConfirmResume, "ConfirmResume");
      WRITE_BOOL(AutoReadDirectoryAfterOp, "AutoReadDirectoryAfterOp");
      WRITE_INT(SessionReopenAuto, "SessionReopenAuto");
      WRITE_INT(SessionReopenTimeout, "SessionReopenTimeout");
      WRITE_INT(SessionReopenAutoStall, "SessionReopenAutoStall");
      WRITE_STRING(ExternalIpAddress, "ExternalIpAddress");
      WRITE_INT(LocalPortNumberMin, "LocalPortNumberMin");
      WRITE_INT(LocalPortNumberMax, "LocalPortNumberMax");
      WRITE_BOOL(TryFtpWhenSshFails, "TryFtpWhenSshFails");
      WRITE_INT(QueueTransfersLimit, "QueueTransfersLimit");
      WRITE_INT(ParallelTransferThreshold, "ParallelTransferThreshold");
      WRITE_BOOL(ContinueOnError, "ContinueOnError");
      WRITE_BOOL(ConfirmCommandSession, "ConfirmCommandSession");
      WRITE_BOOL(QueueKeepDoneItems, "QueueKeepDoneItems");
      WRITE_INT(QueueKeepDoneItemsFor, "QueueKeepDoneItemsFor");
      WRITE_BOOL(QueueAutoPopup, "QueueAutoPopup");
      WRITE_BOOL(SessionRememberPassword, "QueueRememberPassword");
      WRITE_STRING(PuttyPath, "PuttyPath");
      WRITE_BOOL(PuttyPassword, "PuttyPassword");
      WRITE_BOOL(TelnetForFtpInPutty, "TelnetForFtpInPutty");
      WRITE_BOOL(BeepOnFinish, "BeepOnFinish");
      WriteSeconds(Storage, QStringLiteral("BeepOnFinishAfter"), BeepOnFinishAfter);
      WRITE_INT(SessionReopenAutoIdle, "SessionReopenAutoIdle");
      WRITE_BOOL(SessionReopenAutoIdleOn, "SessionReopenAutoIdleOn");
      WRITE_BOOL(SessionReopenAutoInactive, "SessionReopenAutoInactive");
      WRITE_BOOL(SessionSilentDisconnect, "SessionSilentDisconnect");
      WRITE_BOOL(ConfirmExitOnCompletion, "ConfirmExitOnCompletion");
      WRITE_BOOL(SynchronizeSummary, "SynchronizeSummary");
      WRITE_INT(DoubleClickAction, "CopyOnDoubleClick");
      WRITE_BOOL(CopyOnDoubleClickConfirmation, "CopyOnDoubleClickConfirmation");
      WRITE_BOOL(AlwaysRespectDoubleClickAction, "AlwaysRespectDoubleClickAction");
      WRITE_INT(DDTransferConfirmation, "DDTransferConfirmation2");
      WRITE_STRING(DDTemporaryDirectory, "DDTemporaryDirectory");
      WRITE_BOOL(DDWarnLackOfTempSpace, "DDWarnLackOfTempSpace");
      WRITE_BOOL(DeleteToRecycleBin, "DeleteToRecycleBin");
      WRITE_BOOL(RenameWholeName, "RenameWholeName");
      WRITE_BOOL(ShowHiddenFiles, "ShowHiddenFiles");
      WRITE_INT(FormatSizeBytes, "FormatSizeBytes");
      WRITE_INT(PanelSearch, "PanelSearch");
      WRITE_BOOL(ShowInaccesibleDirectories, "ShowInaccesibleDirectories");
      WRITE_BOOL(ConfirmTransferring, "ConfirmTransferring");
      WRITE_BOOL(ConfirmDeleting, "ConfirmDeleting");
      WRITE_BOOL(ConfirmRecycling, "ConfirmRecycling");
      WRITE_BOOL(ConfirmClosingSession, "ConfirmClosingSession");
      WRITE_BOOL(UseLocationProfiles, "UseLocationProfiles");
      WRITE_INT(Locale, "LocaleSafe");
      WRITE_BOOL(DefaultDirIsHome, "DefaultDirIsHome");
      WRITE_BOOL(TemporaryDirectoryAppendSession, "TemporaryDirectoryAppendSession");
      WRITE_BOOL(TemporaryDirectoryAppendPath, "TemporaryDirectoryAppendPath");
      WRITE_BOOL(TemporaryDirectoryDeterministic, "TemporaryDirectoryDeterministic");
      WRITE_BOOL(TemporaryDirectoryCleanup, "TemporaryDirectoryCleanup");
      WRITE_BOOL(ConfirmTemporaryDirectoryCleanup, "ConfirmTemporaryDirectoryCleanup");
      WRITE_BOOL(PreservePanelState, "PreservePanelState");
      WRITE_INT(DarkTheme, "DarkTheme");
      WRITE_BOOL(AutoSaveWorkspace, "AutoSaveWorkspace");
      WRITE_BOOL(AutoSaveWorkspacePasswords, "AutoSaveWorkspacePasswords");
      WRITE_STRING(AutoWorkspace, "AutoWorkspace");
      WRITE_BOOL(WorkspaceConnectAll, "WorkspaceConnectAll");
      WRITE_INT(PathInCaption, "PathInCaption");
      WRITE_INT(SessionTabNameFormat, "SessionTabNameFormat");
      WRITE_BOOL(MinimizeToTray, "MinimizeToTray");
      WRITE_BOOL(BalloonNotifications, "BalloonNotifications");
      WRITE_BOOL(CopyParamAutoSelectNotice, "CopyParamAutoSelectNotice");
      WRITE_BOOL(AutoOpenInPutty, "AutoOpenInPutty");
      WRITE_BOOL(RefreshRemotePanel, "RefreshRemotePanel");
      WriteSeconds(Storage, QStringLiteral("RefreshRemotePanelInterval"), RefreshRemotePanelInterval);
      WRITE_STRING(PanelFontName, "PanelFontName");
      WRITE_INT(PanelFontSize, "PanelFontSize");
      WRITE_BOOL(NaturalOrderNumericalSorting, "NaturalOrderNumericalSorting");
      WRITE_BOOL(AlwaysSortDirectoriesByName, "AlwaysSortDirectoriesByName");
      WRITE_BOOL(FullRowSelect, "FullRowSelect");
      WRITE_BOOL(EnableQueueByDefault, "EnableQueueByDefault");
      WRITE_BOOL(ExternalSessionInExistingInstance, "ExternalSessionInExistingInstance");
      WRITE_BOOL(ShowLoginOnStartup, "ShowLoginWhenNoSession");
      WRITE_BOOL(KeepOpenWhenNoSession, "KeepOpenWhenNoSession");
      WRITE_BOOL(FlashTaskbar, "FlashTaskbar");
      WRITE_BOOL(EditorCheckNotModified, "EditorCheckNotModified");
      WRITE_BOOL(SessionTabCaptionTruncation, "SessionTabCaptionTruncation");

      if (Storage.OpenSubKey(QStringLiteral("CopyParam"), true))
      {
        FCopyParam.Save(Storage);
        Storage.CloseSubKey();
      }
      if (Storage.OpenSubKey(QStringLiteral("Editor"), true))
      {
        WRITE_STRING(EditorFontName, "FontName2");
        WRITE_INT(EditorFontSize, "FontSize");
        WRITE_BOOL(EditorWordWrap, "WordWrap");
        WRITE_INT(EditorTabSize, "TabSize");
        WRITE_BOOL(EditorWarnOrLargeFileSize, "WarnOrLargeFileSize");
        WRITE_INT(EditorLargeFileSize, "LargeFileSize");
        WRITE_INT(EditorEncoding, "Encoding");
        WRITE_INT(EditorFontColor, "FontColor");
        WRITE_INT(EditorBackgroundColor, "BackgroundColor");
        // TEditorList::Save: Storage->ClearSubKeys()
        for (const QString & SubKey : Storage.GetSubKeyNames())
        {
          Storage.RecursiveDeleteSubKey(SubKey);
        }
        for (int Index = 0; Index < Editors.size(); Index++)
        {
          if (Storage.OpenSubKey(QString::number(Index), true))
          {
            const EditorPreferences & E = Editors[Index];
            WRITE_STRING(E.FileMask, "FileMask");
            WRITE_INT(E.Editor, "Editor");
            WRITE_STRING(E.ExternalEditor, "ExternalEditor");
            WRITE_BOOL(E.ExternalEditorText, "ExternalEditorText");
            WRITE_BOOL(E.SDIExternalEditor, "SDIExternalEditor");
            WRITE_BOOL(E.DetectMDIExternalEditor, "DetectMDIExternalEditor");
            Storage.CloseSubKey();
          }
        }
        Storage.CloseSubKey();
      }
      if (Storage.OpenSubKey(QStringLiteral("QueueView"), true))
      {
        WRITE_INT(QueueViewShow, "Show");
        Storage.CloseSubKey();
      }
      if (Storage.OpenSubKey(QStringLiteral("Commander"), true))
      {
        WRITE_BOOL(SwappedPanels, "SwappedPanels");
        WRITE_INT(NortonLikeMode, "ExplorerStyleSelection");
        WRITE_BOOL(PreserveLocalDirectory, "PreserveLocalDirectory");
        WRITE_BOOL(CompareByTime, "CompareByTime");
        WRITE_BOOL(CompareBySize, "CompareBySize");
        WRITE_BOOL(TreeOnLeft, "TreeOnLeft");
        WRITE_BOOL(ExplorerKeyboardShortcuts, "ExplorerKeyboardShortcuts");
        WRITE_BOOL(SystemContextMenu, "SystemContextMenu");
        Storage.CloseSubKey();
      }
      if (Storage.OpenSubKey(QStringLiteral("NixSCP"), true))
      {
        Storage.WriteStringRaw(QStringLiteral("WindowGeometry"), QString::fromLatin1(WindowGeometry.toHex()));
        Storage.WriteStringRaw(QStringLiteral("WindowState"), QString::fromLatin1(WindowState.toHex()));
        Storage.WriteStringRaw(QStringLiteral("PanelSplitter"), QString::fromLatin1(PanelSplitter.toHex()));
        Storage.WriteString(QStringLiteral("LastLocalDirectory"), LastLocalDirectory);
        Storage.WriteBool(QStringLiteral("CheckForUpdates"), CheckForUpdates);
        Storage.WriteString(QStringLiteral("UpdateLastCheck"), UpdateLastCheck.isValid() ? UpdateLastCheck.toUTC().toString(Qt::ISODate) : QString());
        Storage.WriteString(QStringLiteral("UpdateSkipVersion"), UpdateSkipVersion);
        Storage.CloseSubKey();
      }
      Storage.CloseSubKey();
    }
    if (Storage.OpenSubKey(QStringLiteral("Logging"), true))
    {
      SessionLog::PermanentLoggingScope Permanent(*this);
      WRITE_BOOL(Logging, "Logging");
      WRITE_STRING(LogFileName, "LogFileName");
      WRITE_BOOL(LogFileAppend, "LogFileAppend");
      WRITE_BOOL(LogSensitive, "LogSensitive");
      Storage.WriteStringRaw(QStringLiteral("LogMaxSize"), QString::number(LogMaxSize));
      WRITE_INT(LogMaxCount, "LogMaxCount");
      WRITE_INT(LogProtocol, "LogProtocol");
      WRITE_BOOL(LogActions, "LogActions");
      WRITE_STRING(ActionsLogFileName, "ActionsLogFileName");
      Storage.CloseSubKey();
    }
    // TWinConfiguration: BLOCK(L"Security", ...)
    if (Storage.OpenSubKey(QStringLiteral("Security"), true))
    {
      WRITE_BOOL(Security::UseMasterPassword(), "UseMasterPassword");
      WRITE_STRING(Security::MasterPasswordVerifier(), "MasterPasswordVerifier");
      Storage.CloseSubKey();
    }
    #undef WRITE_BOOL
    #undef WRITE_INT
    #undef WRITE_STRING
    Storage.CloseSubKey();
  }
}

//---------------------------------------------------------------------------
IniFile Configuration::LoadSystemConfiguration()
{
  // TConfiguration::Default + LoadAdmin: policies from the machine-wide file
  FDisablePasswordStoring = false;
  FForceBanners = false;
  FDisableAcceptingHostKeys = false;
  FDisableOpenEdit = false;
  IniFile Result;
  const QStringList FileNames = SystemIniFileNames();
  // least important first, so that the more important ones override it
  for (int Index = FileNames.size() - 1; Index >= 0; Index--)
  {
    IniFile System;
    if (!System.Load(FileNames[Index]))
    {
      continue;
    }
    IniStorage Admin(&System);
    FDisablePasswordStoring = Admin.ReadBool(QStringLiteral("DisablePasswordStoring"), FDisablePasswordStoring);
    FForceBanners = Admin.ReadBool(QStringLiteral("ForceBanners"), FForceBanners);
    FDisableAcceptingHostKeys = Admin.ReadBool(QStringLiteral("DisableAcceptingHostKeys"), FDisableAcceptingHostKeys);
    FDisableOpenEdit = Admin.ReadBool(QStringLiteral("DisableOpenEdit"), FDisableOpenEdit);
    // DefaultCollectUsage and DefaultUpdatesPeriod: usage statistics and updates are not part of NixSCP
    System.EraseSection(QString());
    Result.Overlay(System);
  }
  // TSessionData::SavePasswords consults the policy too
  SessionData::DisablePasswordStoring = FDisablePasswordStoring;
  return Result;
}

void Configuration::LoadFromDisk(const QByteArray & DiskData, const QList<IniFile::Change> & Pending, bool Prompt)
{
  // re-reading in the background must not ask for the master password
  // (neither for decrypting, nor for encrypting the canonical form)
  std::unique_ptr<Security::NoMasterPasswordPrompt> NoPrompt(Prompt ? nullptr : new Security::NoMasterPasswordPrompt());
  IniFile Disk;
  Disk.LoadFromData(DiskData);
  // machine-wide defaults below the user's file
  IniFile Effective = LoadSystemConfiguration();
  Effective.Overlay(Disk);

  Default();
  FIni = Effective;
  LoadFrom(FIni, Prompt);
  // The baseline is the configuration as this instance would write it: comparing
  // to it on Save finds exactly what was changed (not what NixSCP writes differently)
  SaveTo(FIni);
  FBaseline = FIni;
  FDiskData = DiskData;

  if (!Pending.isEmpty())
  {
    // unsaved changes of this instance stay
    FIni.Apply(Pending);
    Default();
    LoadFrom(FIni, false);
    SaveTo(FIni);
  }
}

void Configuration::Load()
{
  SessionLog::SetConfigurationLoaded();
  QByteArray DiskData;
  FLocked = false;
  if (!FNulStorage)
  {
    DiskData = ReadFileData(FIniFileName, &FLocked);
  }
  LoadFromDisk(DiskData, QList<IniFile::Change>(), true);
  FLoaded = true;
  StartWatching();
  emit StoredSessionsChanged();
  EmitChangedIfNeeded(true);
}

bool Configuration::Rewrite()
{
  if (FNulStorage)
  {
    return true;
  }
  SaveTo(FIni);
  bool External = false;
  return WriteDisk(QList<IniFile::Change>(), false, IniFile(), External);
}

bool Configuration::WriteDisk(const QList<IniFile::Change> & Changes, bool Replace, const IniFile & Replacement, bool & External)
{
  External = false;
  QDir().mkpath(QFileInfo(FIniFileName).absolutePath());
  // TIniFileStorage::Flush retries on a sharing violation; here the instances
  // serialize the read-merge-write by a lock file (a stale one is taken over)
  QLockFile Lock(FIniFileName + QStringLiteral(".lock"));
  Lock.setStaleLockTime(30 * 1000);
  bool Locked = Lock.tryLock(10 * 1000);

  bool Encrypted = false;
  QByteArray DiskData = ReadFileData(FIniFileName, &Encrypted);
  if (Encrypted)
  {
    // encrypted with a key this process does not have: never overwrite it
    if (Lock.isLocked())
    {
      Lock.unlock();
    }
    FLastSaveError = QStringLiteral("The configuration file \"%1\" is encrypted and locked.").arg(FIniFileName);
    return false;
  }
  External = (DiskData != FDiskData);
  IniFile Disk;
  if (Replace)
  {
    Disk = Replacement;
  }
  else
  {
    Disk.LoadFromData(DiskData);
    Disk.Apply(Changes);
  }
  bool Result = Disk.Save(FIniFileName);
  if (Result)
  {
    FDiskData = Disk.SaveToData();
  }
  else
  {
    FLastSaveError = QStringLiteral("Error writing to file \"%1\".").arg(FIniFileName);
  }
  if (Locked)
  {
    Lock.unlock();
  }
  if (FWatcher != nullptr)
  {
    // the file was replaced (QSaveFile), watch the new one
    StartWatching();
  }
  return Result;
}

bool Configuration::Save()
{
  FLastSaveError.clear();
  if (!FLoaded)
  {
    // Save without Load: only what differs from the defaults is written
    // (merged into the file, which is re-read then)
    ConfigurationData Current = *this;
    CopyParam CurrentCopyParam = FCopyParam;
    Default();
    IniFile Defaults;
    SaveTo(Defaults);
    static_cast<ConfigurationData &>(*this) = Current;
    FCopyParam = CurrentCopyParam;
    FBaseline = Defaults;
    // anything differing from the disk is "external"
    FDiskData = QByteArray();
    FLoaded = true;
  }
  SaveTo(FIni);
  if (FNulStorage)
  {
    // nothing is written; the "file" is kept in memory (for Export/MoveStorage)
    FDiskData = UserConfiguration(FDiskData, FBaseline, FIni).SaveToData();
    FBaseline = FIni;
    EmitChangedIfNeeded(false);
    return true;
  }
  QList<IniFile::Change> Changes = IniFile::Diff(FBaseline, FIni, PasswordEquivalent);
  if (Changes.isEmpty() && QFileInfo::exists(FIniFileName))
  {
    EmitChangedIfNeeded(false);
    return true;
  }
  bool External = false;
  if (!WriteDisk(Changes, false, IniFile(), External))
  {
    // the baseline is kept, so the changes are written by the next Save
    return false;
  }
  if (External)
  {
    // another instance saved meanwhile: re-read the merged file, so that
    // this instance sees its changes too
    LoadFromDisk(FDiskData, QList<IniFile::Change>(), false);
    emit StoredSessionsChanged();
  }
  else
  {
    FBaseline = FIni;
  }
  EmitChangedIfNeeded(false);
  emit Saved();
  return true;
}

bool Configuration::Reload()
{
  if (FNulStorage || !FLoaded)
  {
    return false;
  }
  QByteArray DiskData = ReadFileData(FIniFileName);
  if (DiskData == FDiskData)
  {
    return false;
  }
  QList<IniFile::Change> Pending;
  {
    Security::NoMasterPasswordPrompt NoPrompt;
    SaveTo(FIni);
    Pending = IniFile::Diff(FBaseline, FIni, PasswordEquivalent);
  }
  LoadFromDisk(DiskData, Pending, false);
  emit StoredSessionsChanged();
  EmitChangedIfNeeded(false);
  return true;
}

void Configuration::EmitChangedIfNeeded(bool Force)
{
  ConfigurationData Current = *this;
  // window state is saved on exit, that is not a preference change
  Current.WindowGeometry.clear();
  Current.WindowState.clear();
  Current.PanelSplitter.clear();
  Current.LastLocalDirectory.clear();
  // nor is the time of the last new version check
  Current.UpdateLastCheck = QDateTime();
  if (Force || !FChangedValid || !(Current == FChangedData) || !(FCopyParam == FChangedCopyParam))
  {
    FChangedData = Current;
    FChangedCopyParam = FCopyParam;
    FChangedValid = true;
    emit Changed();
  }
}

void Configuration::StartWatching()
{
  if (FNulStorage || (QCoreApplication::instance() == nullptr) ||
      (thread() != QCoreApplication::instance()->thread()))
  {
    return;
  }
  if (FWatcher == nullptr)
  {
    FWatcher = new QFileSystemWatcher(this);
    FReloadTimer = new QTimer(this);
    FReloadTimer->setSingleShot(true);
    FReloadTimer->setInterval(300);
    connect(FWatcher, &QFileSystemWatcher::fileChanged, FReloadTimer, qOverload<>(&QTimer::start));
    connect(FWatcher, &QFileSystemWatcher::directoryChanged, FReloadTimer, qOverload<>(&QTimer::start));
    connect(FReloadTimer, &QTimer::timeout, this, &Configuration::CheckFileChanged);
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this]()
    {
      // before the event dispatcher goes away
      delete FWatcher;
      FWatcher = nullptr;
      FReloadTimer->stop();
    });
  }
  QStringList Paths = { QFileInfo(FIniFileName).absolutePath() };
  if (QFileInfo::exists(FIniFileName))
  {
    Paths.append(FIniFileName);
  }
  QStringList Watched = FWatcher->files() + FWatcher->directories();
  for (const QString & Path : Watched)
  {
    if (!Paths.contains(Path))
    {
      FWatcher->removePath(Path);
    }
  }
  for (const QString & Path : Paths)
  {
    if (!Watched.contains(Path))
    {
      FWatcher->addPath(Path);
    }
  }
}

void Configuration::CheckFileChanged()
{
  if (QThread::currentThread()->loopLevel() > 1)
  {
    // a modal dialog (or another nested event loop) may be iterating the
    // site list; reload once it is closed
    FReloadTimer->start(1000);
    return;
  }
  StartWatching();
  Reload();
}

//---------------------------------------------------------------------------
bool Configuration::MoveStorage(const QString & CustomIniFileName, bool UseExisting, QString * Error)
{
  QString Target = CustomIniFileName.trimmed().isEmpty() ? AutomaticIniFileName() : ExpandPath(CustomIniFileName);
  bool Same = !FNulStorage && (QFileInfo(Target).absoluteFilePath() == QFileInfo(FIniFileName).absoluteFilePath());
  if (!Same && !UseExisting)
  {
    // CopyData + DoSave(All, Explicit): the current configuration over
    // whatever the target file contains
    SaveTo(FIni);
    IniFile Current = UserConfiguration(FNulStorage ? FDiskData : ReadFileData(FIniFileName), FBaseline, FIni);
    IniFile Out;
    Out.Load(Target);
    Out.Overlay(Current);
    QDir().mkpath(QFileInfo(Target).absolutePath());
    if (!Out.Save(Target))
    {
      if (Error != nullptr)
      {
        *Error = QStringLiteral("Error writing to file \"%1\".").arg(Target);
      }
      return false;
    }
  }
  FCustomIniFileName = CustomIniFileName.trimmed();
  SaveOverride();
  if (!Same)
  {
    FExplicitIniFileName.clear();
    FNulStorage = false;
    FIniFileName = Target;
    Load();
  }
  return true;
}

QString Configuration::DefaultIniFileExportPath() const
{
  QString Dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
  if (Dir.isEmpty() || !QFileInfo(Dir).isDir())
  {
    Dir = QDir::homePath();
  }
  QString Name = FNulStorage ? QStringLiteral("nixscp.ini") : QFileInfo(FIniFileName).fileName();
  return Dir + QLatin1Char('/') + Name;
}

IniFile Configuration::ExportData()
{
  SaveTo(FIni);
  return UserConfiguration(FNulStorage ? FDiskData : ReadFileData(FIniFileName), FBaseline, FIni);
}

bool Configuration::Export(const QString & FileName, QString * Error)
{
  // TConfiguration::Export: complete configuration to a new file
  IniFile Out = ExportData();
  QFile::remove(FileName);
  if (!Out.Save(FileName))
  {
    if (Error != nullptr)
    {
      *Error = QStringLiteral("Error writing to file \"%1\".").arg(FileName);
    }
    return false;
  }
  return true;
}

bool Configuration::Import(const QString & FileName, QString * Error)
{
  // TConfiguration::Import: the backup replaces all preferences and sites
  IniFile In;
  if (!QFileInfo(FileName).isFile() || !In.Load(FileName))
  {
    if (Error != nullptr)
    {
      *Error = QStringLiteral("Cannot open file \"%1\".").arg(FileName);
    }
    return false;
  }
  return ImportData(In, Error);
}

bool Configuration::ImportData(IniFile In, QString * Error)
{
  In.EraseSection(QString());
  // A cloud sync key in a backup is taken by CloudSync::ImportKey; it never
  // becomes part of the configuration file
  In.EraseSectionTree(CloudSettings::KeySection);
  if (FNulStorage)
  {
    LoadFromDisk(In.SaveToData(), QList<IniFile::Change>(), true);
  }
  else
  {
    bool External = false;
    if (!WriteDisk(QList<IniFile::Change>(), true, In, External))
    {
      if (Error != nullptr)
      {
        *Error = FLastSaveError;
      }
      return false;
    }
    LoadFromDisk(FDiskData, QList<IniFile::Change>(), true);
  }
  emit StoredSessionsChanged();
  EmitChangedIfNeeded(true);
  return true;
}

//---------------------------------------------------------------------------
bool Configuration::UseMasterPassword() const
{
  return Security::UseMasterPassword();
}

// TWinConfiguration::RecryptPasswords: re-reads the sites (passwords that could
// not be decrypted before, can be decrypted now) and reports the ones that cannot
static void RecryptErrors(StoredSessionList & Sessions, const QByteArray & DiskData, QStringList * Errors)
{
  if (Errors == nullptr)
  {
    return;
  }
  IniFile Disk;
  Disk.LoadFromData(DiskData);
  for (const auto & Data : Sessions.Sessions())
  {
    if (Data->Password.isEmpty())
    {
      QString Section = SessionsSectionPrefix + MungeStr(Data->Name, false);
      QByteArray Stored = Disk.ReadRaw(Section, QStringLiteral("Password")).toLatin1();
      if (Security::IsStronglyEncrypted(Stored))
      {
        Errors->append(QStringLiteral("%1: The password cannot be decrypted.").arg(Data->Name));
      }
    }
  }
}

bool Configuration::ChangeMasterPassword(const QString & NewPassword, QStringList * Errors)
{
  // TWinConfiguration::ChangeMasterPassword
  SaveTo(FIni);
  QList<IniFile::Change> Pending = IniFile::Diff(FBaseline, FIni, PasswordEquivalent);
  LoadFromDisk(FNulStorage ? QByteArray() : ReadFileData(FIniFileName), Pending, false);
  RecryptErrors(FStoredSessions, FDiskData, Errors);
  Security::ChangeMasterPassword(NewPassword);
  // decrypt with the new password from now on; before Save, as Save re-reads
  // the file (encrypted with the new password), if another instance saved meanwhile
  Security::SetMasterPassword(NewPassword);
  return Save();
}

bool Configuration::ClearMasterPassword(QStringList * Errors)
{
  // TWinConfiguration::ClearMasterPassword
  SaveTo(FIni);
  QList<IniFile::Change> Pending = IniFile::Diff(FBaseline, FIni, PasswordEquivalent);
  LoadFromDisk(FNulStorage ? QByteArray() : ReadFileData(FIniFileName), Pending, false);
  RecryptErrors(FStoredSessions, FDiskData, Errors);
  Security::ClearMasterPassword();
  bool Result = Save();
  Security::SetMasterPassword(QString());
  return Result;
}

QString Configuration::ExpandedTemporaryDirectory() const
{
  // TWinConfiguration::ExpandedTemporaryDirectory
  QString Result = DDTemporaryDirectory.trimmed().isEmpty() ? QDir::tempPath() : ExpandPath(DDTemporaryDirectory);
  while ((Result.size() > 1) && Result.endsWith(QLatin1Char('/')))
  {
    Result.chop(1);
  }
  return Result;
}
