// Port of WinSCP source/core/Configuration.cpp + windows/GUIConfiguration.cpp +
// windows/CustomWinConfiguration.cpp + windows/WinConfiguration.cpp (subset)
// and the storage part of core/HierarchicalStorage.cpp.
//
// Everything is kept in one WinSCP.ini compatible file. Its location
// (TConfiguration::IniFileStorageName):
//  - explicit: SetIniFileName (/ini=<file>; /ini=nul keeps nothing),
//  - custom: Preferences > Storage > Custom INI file (remembered in
//    ~/.config/nixscp/override.ini, as WinSCP keeps it in the "WinSCP 2 Override" key),
//  - automatic: nixscp.ini next to the executable or the AppImage (portable mode),
//    otherwise ~/.config/nixscp/nixscp.ini.
// Machine-wide defaults and policies are read (read-only) from nixscp/nixscp.ini
// in $XDG_CONFIG_DIRS (/etc/xdg/nixscp/nixscp.ini): its sections provide defaults
// below the user's file; values before the first section are the policies of
// WinSCP's HKLM key (DisablePasswordStoring, ForceBanners, DisableAcceptingHostKeys,
// DisableOpenEdit).
//
// Several running instances can share the file: Save writes only what this
// instance changed, merged into the file as it is on disk at that moment
// (under a lock file), and the file is watched for changes by other instances.
// Keys NixSCP does not know (e.g. Windows-only ones) are left untouched.
//
// Key names and sections match WinSCP ("Configuration\Interface",
// "Configuration\Interface\Commander", "Configuration\Logging", "Configuration\Security"...).
#pragma once

#include "core/IniStorage.h"
#include "core/SessionData.h"
#include "core/CopyParam.h"

#include <QByteArray>
#include <QList>
#include <QObject>

class QFileSystemWatcher;
class QTimer;

// windows/WinConfiguration.h
enum TEditor { edInternal = 0, edExternal = 1, edOpen = 2 };
enum TDoubleClickAction { dcaOpen = 0, dcaCopy = 1, dcaEdit = 2 };
enum TPathInCaption { picShort = 0, picFull = 1, picNone = 2 };
enum TQueueViewShow { qvShow = 0, qvHideWhenEmpty = 1, qvHide = 2 };
enum TSessionTabNameFormat { stnfNone = 0, stnfShortPath = 1, stnfShortPathTrunc = 2 };
// packages/filemng/BaseUtils.pas, packages/my/NortonLikeListView.pas
enum TFormatBytesStyle { fbNone = 0, fbKilobytes = 1, fbShort = 2 };
enum TNortonLikeMode { nlOn = 0, nlOff = 1, nlKeyboard = 2 };
// windows/CustomWinConfiguration.h
enum TIncrementalSearch { isOff = -1, isNameStartOnly = 0, isName = 1, isAll = 2 };

// TEditorData / TEditorPreferences; stored in Configuration\Interface\Editor\<n>
struct EditorPreferences
{
  QString FileMask = QStringLiteral("*.*");   // FileMask
  TEditor Editor = edInternal;                // Editor
  QString ExternalEditor;                     // ExternalEditor (command; "!.!" = file name)
  bool ExternalEditorText = false;            // ExternalEditorText (text transfer mode)
  bool SDIExternalEditor = false;             // SDIExternalEditor
  bool DetectMDIExternalEditor = false;       // DetectMDIExternalEditor

  // Caption as shown in Preferences ("Internal editor", program name, "System default")
  QString Name() const;
  bool operator==(const EditorPreferences & Other) const = default;
};

// Preference values (properties of TConfiguration, TGUIConfiguration,
// TCustomWinConfiguration and TWinConfiguration); Configuration::Default() resets them
struct ConfigurationData
{
  ConfigurationData();
  bool operator==(const ConfigurationData & Other) const = default;

  // ---- Interface state (NixSCP only, Configuration\Interface\NixSCP) ----
  QByteArray WindowGeometry;
  QByteArray WindowState;
  QByteArray PanelSplitter;
  QString LastLocalDirectory;

  // ---- Environment: confirmations / notifications ----
  // ConfirmOverwriting and ConfirmResume live in DefaultCopyParam()
  bool ConfirmDeleting = true;                 // ConfirmDeleting
  bool ConfirmRecycling = true;                // ConfirmRecycling
  bool ConfirmClosingSession = true;           // ConfirmClosingSession
  bool ConfirmExitOnCompletion = true;         // ConfirmExitOnCompletion
  bool ConfirmCommandSession = true;           // ConfirmCommandSession
  bool ConfirmTransferring = true;             // ConfirmTransferring
  TAutoSwitch DDTransferConfirmation = asAuto; // DDTransferConfirmation2
  bool ContinueOnError = false;                // ContinueOnError
  bool SynchronizeSummary = true;              // SynchronizeSummary
  bool BeepOnFinish = false;                   // BeepOnFinish
  int BeepOnFinishAfter = 30;                  // BeepOnFinishAfter (seconds; stored as TDateTime)
  bool BalloonNotifications = true;            // BalloonNotifications
  bool FlashTaskbar = true;                    // FlashTaskbar

  // ---- Environment > Interface ----
  TAutoSwitch DarkTheme = asAuto;              // DarkTheme (Automatic/Light(off)/Dark(on))

  // ---- Environment > Window ----
  TPathInCaption PathInCaption = picShort;     // PathInCaption
  TSessionTabNameFormat SessionTabNameFormat = stnfShortPathTrunc; // SessionTabNameFormat (hidden)
  bool MinimizeToTray = false;                 // MinimizeToTray
  bool ExternalSessionInExistingInstance = true; // ExternalSessionInExistingInstance
  bool ShowLoginOnStartup = true;              // ShowLoginWhenNoSession
  bool KeepOpenWhenNoSession = true;           // KeepOpenWhenNoSession
  bool SessionTabCaptionTruncation = true;     // SessionTabCaptionTruncation
  bool SessionSilentDisconnect = false;        // SessionSilentDisconnect
  bool AutoSaveWorkspace = false;              // AutoSaveWorkspace
  bool AutoSaveWorkspacePasswords = false;     // AutoSaveWorkspacePasswords
  QString AutoWorkspace;                       // AutoWorkspace
  bool WorkspaceConnectAll = false;            // WorkspaceConnectAll

  // ---- Environment > Commander (Configuration\Interface\Commander) ----
  bool SwappedPanels = false;                  // SwappedPanels
  TNortonLikeMode NortonLikeMode = nlKeyboard; // ExplorerStyleSelection
  bool TreeOnLeft = false;                     // TreeOnLeft
  bool ExplorerKeyboardShortcuts = false;      // ExplorerKeyboardShortcuts
  bool CompareByTime = true;                   // CompareByTime
  bool CompareBySize = false;                  // CompareBySize
  bool PreserveLocalDirectory = false;         // PreserveLocalDirectory (Local page)
  bool SystemContextMenu = false;              // SystemContextMenu (Local page)
  bool UseLocationProfiles = false;            // UseLocationProfiles (Configuration\Interface)

  // ---- Environment > Languages ----
  // LocaleSafe: Windows LCID of the UI language, 0 = system default
  // (see PreferencesDialog for the LCID <-> translation mapping)
  int Locale = 0;

  // ---- Panels ----
  bool ShowHiddenFiles = false;                // ShowHiddenFiles
  bool DefaultDirIsHome = true;                // DefaultDirIsHome
  bool PreservePanelState = true;              // PreservePanelState
  bool RenameWholeName = false;                // RenameWholeName
  bool FullRowSelect = false;                  // FullRowSelect
  TFormatBytesStyle FormatSizeBytes = fbKilobytes; // FormatSizeBytes
  bool NaturalOrderNumericalSorting = true;    // NaturalOrderNumericalSorting
  TIncrementalSearch PanelSearch = isNameStartOnly; // PanelSearch
  bool AlwaysSortDirectoriesByName = false;    // AlwaysSortDirectoriesByName
  TDoubleClickAction DoubleClickAction = dcaEdit; // CopyOnDoubleClick (enum!)
  // Legacy NixSCP flag == (DoubleClickAction == dcaCopy); kept in sync on Load/Save
  bool CopyOnDoubleClick = false;
  bool CopyOnDoubleClickConfirmation = false;  // CopyOnDoubleClickConfirmation
  bool AlwaysRespectDoubleClickAction = false; // AlwaysRespectDoubleClickAction (hidden)
  QString PanelFontName;                       // PanelFontName (empty = default font)
  int PanelFontSize = 0;                       // PanelFontSize (points)
  // Remote
  bool ShowInaccesibleDirectories = true;      // ShowInaccesibleDirectories
  bool AutoReadDirectoryAfterOp = true;        // AutoReadDirectoryAfterOp
  bool RefreshRemotePanel = false;             // RefreshRemotePanel
  int RefreshRemotePanelInterval = 60;         // RefreshRemotePanelInterval (seconds; TDateTime)
  // Local
  bool DeleteToRecycleBin = true;              // DeleteToRecycleBin (local: move to trash)

  // ---- Editors (Configuration\Interface\Editor) ----
  QList<EditorPreferences> Editors;            // Editor\0, Editor\1, ...
  bool EditorCheckNotModified = false;         // EditorCheckNotModified (Interface)
  bool EditorWordWrap = false;                 // WordWrap
  int EditorTabSize = 8;                       // TabSize
  QString EditorFontName;                      // FontName2 (empty = default fixed font)
  int EditorFontSize = 0;                      // FontSize
  bool EditorWarnOrLargeFileSize = true;       // WarnOrLargeFileSize
  int EditorLargeFileSize = 10 * 1024;         // LargeFileSize (KB)
  int EditorEncoding = 65001;                  // Encoding (CP_UTF8 = 65001, CP_ACP = 0)
  int EditorFontColor = 0;                     // FontColor (BGR TColor, 0 = default)
  int EditorBackgroundColor = 0;               // BackgroundColor (BGR TColor, 0 = default)

  // ---- Transfer ----
  bool CopyParamAutoSelectNotice = true;       // CopyParamAutoSelectNotice
  bool DDWarnLackOfTempSpace = true;           // DDWarnLackOfTempSpace (Drag & Drop)
  QString DDTemporaryDirectory;                // DDTemporaryDirectory (empty = system)
  // Background
  int QueueTransfersLimit = 2;                 // QueueTransfersLimit
  bool EnableQueueByDefault = true;            // EnableQueueByDefault
  bool QueueAutoPopup = true;                  // QueueAutoPopup
  bool QueueKeepDoneItems = true;              // QueueKeepDoneItems
  int QueueKeepDoneItemsFor = 15;              // QueueKeepDoneItemsFor (seconds, -1 = forever)
  int ParallelTransferThreshold = -1;          // ParallelTransferThreshold (KB; -1 default/off)
  TQueueViewShow QueueViewShow = qvHideWhenEmpty; // Interface\QueueView\Show
  // Endurance (resume settings are in DefaultCopyParam())
  int SessionReopenAuto = 5000;                // SessionReopenAuto (ms, 0 = off)
  int SessionReopenAutoIdle = 9000;            // SessionReopenAutoIdle (ms)
  bool SessionReopenAutoIdleOn = true;         // SessionReopenAutoIdleOn
  bool SessionReopenAutoInactive = false;      // SessionReopenAutoInactive
  int SessionReopenAutoStall = 60000;          // SessionReopenAutoStall (ms, 0 = off)
  int SessionReopenTimeout = 0;                // SessionReopenTimeout (ms, 0 = forever)

  // ---- Storage > Temporary directory ----
  bool TemporaryDirectoryAppendSession = false;   // TemporaryDirectoryAppendSession
  bool TemporaryDirectoryAppendPath = true;       // TemporaryDirectoryAppendPath
  bool TemporaryDirectoryDeterministic = false;   // TemporaryDirectoryDeterministic
  bool TemporaryDirectoryCleanup = true;          // TemporaryDirectoryCleanup
  bool ConfirmTemporaryDirectoryCleanup = true;   // ConfirmTemporaryDirectoryCleanup

  // ---- Network ----
  QString ExternalIpAddress;                   // ExternalIpAddress
  int LocalPortNumberMin = 0;                  // LocalPortNumberMin
  int LocalPortNumberMax = 0;                  // LocalPortNumberMax
  bool TryFtpWhenSshFails = true;              // TryFtpWhenSshFails

  // ---- Security ----
  // (UseMasterPassword / MasterPasswordVerifier are kept by core/Security.h)
  bool SessionRememberPassword = true;         // QueueRememberPassword

  // ---- Logging (Configuration\Logging) ----
  bool Logging = false;                        // Logging
  QString LogFileName;                         // LogFileName ("!" patterns as WinSCP)
  bool LogFileAppend = true;                   // LogFileAppend
  int LogProtocol = 0;                         // LogProtocol: -1 Reduced, 0 Normal, 1 Debug 1, 2 Debug 2
  bool LogSensitive = false;                   // LogSensitive
  qint64 LogMaxSize = 0;                       // LogMaxSize (bytes, 0 = no rotation)
  int LogMaxCount = 0;                         // LogMaxCount (0 = keep all)
  bool LogActions = false;                     // LogActions (XML log)
  QString ActionsLogFileName;                  // ActionsLogFileName

  // ---- Integration > Applications ----
  QString PuttyPath;                           // PuttyPath (terminal client command)
  bool PuttyPassword = false;                  // PuttyPassword
  bool AutoOpenInPutty = false;                // AutoOpenInPutty
  bool TelnetForFtpInPutty = true;             // TelnetForFtpInPutty
};

class Configuration : public QObject, public ConfigurationData
{
  Q_OBJECT

public:
  static Configuration & Instance();

  // Loads the configuration file (and the machine-wide defaults and policies)
  void Load();
  // Merges the changes made since the last Load/Save into the file on disk;
  // false when the file cannot be written (LastSaveError)
  bool Save();
  // Re-reads the file when another instance changed it; unsaved changes are kept.
  // Called automatically (file watcher), when the application has an event loop.
  bool Reload();
  // TConfiguration::Default: factory defaults (not saved)
  void Default();
  QString LastSaveError() const { return FLastSaveError; }

  // ---- Storage ----
  QString IniFileName() const { return FIniFileName; }
  // Explicit configuration file (/ini=<file>, tests), "nul" = none (do not load or save);
  // does not load it
  void SetIniFileName(const QString & FileName);
  bool IsNulStorage() const { return FNulStorage; }
  QString ExplicitIniFileStorageName() const { return FExplicitIniFileName; }
  QString CustomIniFileStorageName() const { return FCustomIniFileName; }
  // Automatic INI file: portable nixscp.ini or ~/.config/nixscp/nixscp.ini
  static QString AutomaticIniFileName();
  static QString PortableIniFileName();   // empty when not in portable mode
  static QString OverrideFileName();
  // TConfiguration::MoveStorage: switches to the custom INI file (empty = automatic).
  // UseExisting: load the configuration from the existing file ("Use"), otherwise the
  // current configuration is copied to the file ("Overwrite"). Reloads the configuration.
  bool MoveStorage(const QString & CustomIniFileName, bool UseExisting, QString * Error = nullptr);
  // TConfiguration::GetDefaultIniFileExportPath
  QString DefaultIniFileExportPath() const;
  // Tools > Export/Backup Configuration, Import/Restore Configuration.
  // Import replaces the whole configuration (preferences, sites, bookmarks...)
  // and reloads it (no restart needed, Changed and StoredSessionsChanged are emitted).
  bool Export(const QString & FileName, QString * Error = nullptr);
  bool Import(const QString & FileName, QString * Error = nullptr);
  // The same in memory (NixSCP Pro cloud sync): the complete user configuration,
  // and replacing it (the root policies section of Data is ignored)
  IniFile ExportData();
  bool ImportData(IniFile Data, QString * Error = nullptr);
  // Machine-wide files (for tests; default: nixscp/nixscp.ini in $XDG_CONFIG_DIRS)
  QStringList SystemIniFileNames() const;
  void SetSystemIniFileNames(const QStringList & FileNames);

  // ---- Policies (read-only, from the machine-wide file) ----
  bool DisablePasswordStoring() const { return FDisablePasswordStoring; }
  bool ForceBanners() const { return FForceBanners; }
  bool DisableAcceptingHostKeys() const { return FDisableAcceptingHostKeys; }
  bool DisableOpenEdit() const { return FDisableOpenEdit; }

  // ---- Master password (TWinConfiguration) ----
  bool UseMasterPassword() const;
  // Sets a new master password and re-encrypts (and saves) all stored passwords.
  // The current master password (if any) has to be known (Security::SetMasterPassword).
  bool ChangeMasterPassword(const QString & NewPassword, QStringList * RecryptErrors = nullptr);
  bool ClearMasterPassword(QStringList * RecryptErrors = nullptr);

  // ---- Temporary directory (TWinConfiguration::ExpandedTemporaryDirectory) ----
  QString ExpandedTemporaryDirectory() const;

  StoredSessionList & StoredSessions() { return FStoredSessions; }
  // Whole WinSCP.ini compatible file, for components keeping their own
  // sections (bookmarks, history...); saved by Save()
  IniFile & Ini() { return FIni; }
  CopyParam & DefaultCopyParam() { return FCopyParam; }

  // Editor to use for FileName (remote or local name; matched against the
  // masks of the Editors list, first match wins, as TEditorList::Find):
  // returns empty string for the internal editor, otherwise a command line
  // template, where "!.!" stands for the (local) file name. "System default
  // application" (edOpen) returns "xdg-open !.!".
  QString EditorFor(const QString & FileName) const;
  const EditorPreferences * EditorPreferencesFor(const QString & FileName) const;
  // Expands "!.!" in Command with the quoted LocalFileName (appends it when
  // the pattern is missing), like ExternalEditor handling in WinSCP
  static QString FormatEditorCommand(const QString & Command, const QString & LocalFileName);
  static QList<EditorPreferences> DefaultEditors();

  static QString DefaultLogFileName();
  static QString DefaultPuttyPath();

signals:
  // Preferences changed (Load, Save of changed preferences, Reload, Import):
  // windows and panels re-apply them
  void Changed();
  // The site list was re-read (another instance saved, Import, MoveStorage);
  // previously obtained SessionData pointers are not part of the list anymore
  void StoredSessionsChanged();
  // Changes (preferences or sites) were written to the file
  void Saved();

private:
  Configuration();

  void LoadFrom(IniFile & File, bool Prompt);
  void SaveTo(IniFile & File);
  void LoadFromDisk(const QByteArray & DiskData, const QList<IniFile::Change> & Pending, bool Prompt);
  IniFile LoadSystemConfiguration();
  bool WriteDisk(const QList<IniFile::Change> & Changes, bool Replace, const IniFile & Replacement, bool & External);
  void EmitChangedIfNeeded(bool Force);
  void StartWatching();
  void CheckFileChanged();
  void SaveOverride();

  QString FIniFileName;
  QString FExplicitIniFileName;
  QString FCustomIniFileName;
  bool FNulStorage = false;
  QStringList FSystemIniFileNames;
  bool FSystemIniFileNamesSet = false;
  // working copy (all sections, as other components see it)
  IniFile FIni;
  // canonical form of the configuration as last loaded/saved
  IniFile FBaseline;
  // user's file content as last read/written by this instance
  QByteArray FDiskData;
  bool FLoaded = false;
  QString FLastSaveError;
  StoredSessionList FStoredSessions;
  CopyParam FCopyParam;
  // state last announced by Changed
  ConfigurationData FChangedData;
  CopyParam FChangedCopyParam;
  bool FChangedValid = false;
  // policies
  bool FDisablePasswordStoring = false;
  bool FForceBanners = false;
  bool FDisableAcceptingHostKeys = false;
  bool FDisableOpenEdit = false;
  QFileSystemWatcher * FWatcher = nullptr;
  QTimer * FReloadTimer = nullptr;
};
