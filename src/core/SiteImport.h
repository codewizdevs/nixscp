// Port of the site import parts of WinSCP:
//   core/Configuration.cpp   TConfiguration::Select*SessionsForImport, GetOpensshFolder
//   windows/GUIConfiguration.cpp TGUIConfiguration::SelectPuttySessionsForImport
//   core/SessionData.cpp     TSessionData::ImportFromOpenssh, ImportFromFilezilla,
//                            ParseOpensshDirective, CutOpensshToken,
//                            TStoredSessionList::ImportFromOpenssh, ImportFromFilezilla,
//                            ImportFromKnownHosts, SelectSessionsToImport, ImportHostKeys,
//                            ImportSelectedKnownHosts, SelectKnownHostsForSelectedSessions
// Linux adaptation (see docs/PARITY.md, Windows policy):
//   PuTTY sites       ~/.putty/sessions/<name>, host keys ~/.putty/sshhostkeys
//   FileZilla         $XDG_CONFIG_HOME/filezilla/sitemanager.xml (or ~/.filezilla)
//   OpenSSH           ~/.ssh/config (Include directives, globs), ~/.ssh/known_hosts
//   KiTTY             dropped (Windows only)
//   Host key cache    imported host keys (PuTTY format of PuTTY and WinSCP.ini
//                     [SshHostKeys]) are converted to OpenSSH known_hosts lines,
//                     as NixSCP verifies host keys against known_hosts.
//   Windows paths     C:\... key files and local directories of imported sites
//                     can be remapped to the home folder (RemapWindowsPaths).
#pragma once

#include "core/SessionData.h"

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <memory>

// TStoredSessionList used for import (sites + their Selected flag)
struct ImportSessionList
{
  QList<std::shared_ptr<SessionData>> Sessions;
  QList<bool> Selected;
  // known_hosts source only: the OpenSSH known_hosts lines of each entry
  QList<QStringList> KnownHostLines;
  // Error shown instead of the list (empty list) - FILEZILLA_NO_SITES etc.
  QString Error;

  int Count() const { return static_cast<int>(Sessions.size()); }
  void Add(std::shared_ptr<SessionData> Data, bool ASelected = false);
  std::shared_ptr<SessionData> FindByName(const QString & Name) const;
  QList<std::shared_ptr<SessionData>> SelectedSessions() const;
  // TStoredSessionList::SelectSessionsToImport: SSH sites not yet stored
  void SelectSessionsToImport(const StoredSessionList & Dest, bool SshOnly);
};

namespace SiteImport
{
  // Order of the Import dialog source combo (ImportSessions.dfm, without KiTTY)
  enum TSource { isPutty = 0, isFilezilla = 1, isOpenssh = 2, isIni = 3, isKnownHosts = 4, isCount = 5 };

  // Default locations (GetOpensshFolder, ...)
  QString OpensshFolder();
  QString OpensshConfigFile();
  QString KnownHostsFile();
  QString PuttyFolder();
  QString PuttySessionsFolder();
  QString PuttyHostKeysFile();
  QString FilezillaFolder();

  // OpenSSH config syntax
  bool ParseOpensshDirective(const QString & Line, QString & Directive, QString & Value);
  QString CutOpensshToken(QString & S);
  QString ConvertPathFromOpenssh(const QString & Path);
  // Reads the config, replacing Include directives with the included files
  // (relative to ~/.ssh, globs allowed, nested); Error set when FileName is missing
  QStringList LoadOpensshConfig(const QString & FileName, QString * Error = nullptr);
  // TSessionData::ImportFromOpenssh: settings of Host blocks matching Data.Name
  void ImportFromOpenssh(SessionData & Data, const QStringList & Lines);

  // Import sources (TConfiguration::Select*SessionsForImport). Sessions are
  // based on Stored.DefaultSettings(); Error is set when there is nothing to import.
  ImportSessionList SelectPuttySessionsForImport(const StoredSessionList & Stored, const QString & SessionsFolder = PuttySessionsFolder());
  ImportSessionList SelectFilezillaSessionsForImport(const StoredSessionList & Stored, const QString & Folder = FilezillaFolder());
  ImportSessionList SelectOpensshSessionsForImport(const StoredSessionList & Stored, const QString & ConfigFile = OpensshConfigFile());
  ImportSessionList SelectSessionsForImport(const StoredSessionList & Stored, const QString & IniFileName);
  ImportSessionList SelectKnownHostsSessionsForImport(const StoredSessionList & Stored, const QStringList & Lines,
    const QString & TargetKnownHostsFile = KnownHostsFile());
  ImportSessionList SelectKnownHostsSessionsForImportFromFile(const StoredSessionList & Stored, const QString & FileName = KnownHostsFile(),
    const QString & TargetKnownHostsFile = KnownHostsFile());
  // TGUIConfiguration::AnyPuttySessionForImport etc. (first-run import offer)
  bool AnyPuttySessionForImport(const StoredSessionList & Stored);
  bool AnyFilezillaSessionForImport(const StoredSessionList & Stored);
  bool AnyOpensshSessionForImport(const StoredSessionList & Stored);

  // Host keys. PuTTY cache format: name "<cache id>@<port>:<host>"
  // (rsa2, dss, ecdsa-sha2-nistp256, ssh-ed25519, ...), value "0x..,0x..".
  QString PuttyHostKeyToKnownHostsLine(const QString & KeyName, const QString & Value, QString * Error = nullptr);
  QMap<QString, QString> LoadPuttyHostKeys(const QString & FileName = PuttyHostKeysFile());
  QMap<QString, QString> LoadIniHostKeys(const QString & IniFileName);
  // TStoredSessionList::ImportHostKeys: keys of the sessions' host:port are
  // converted and added to KnownHostsFile; returns the number of keys added
  int ImportHostKeys(const QMap<QString, QString> & HostKeys, const QList<std::shared_ptr<SessionData>> & Sessions,
    const QString & TargetKnownHostsFile = KnownHostsFile());
  // ImportSelectedKnownHosts: adds selected entries' lines; returns number added
  int ImportSelectedKnownHosts(const ImportSessionList & KnownHosts, const QString & TargetKnownHostsFile = KnownHostsFile());
  // TStoredSessionList::SelectKnownHostsForSelectedSessions
  void SelectKnownHostsForSelectedSessions(ImportSessionList & KnownHosts, const ImportSessionList & Sessions);
  // Appends lines whose key is not in the file yet (creates ~/.ssh 0700, file 0600)
  int AppendKnownHosts(const QString & TargetKnownHostsFile, const QStringList & Lines, QString * Error = nullptr);
  // known_hosts host field for host:port ("host" or "[host]:port")
  QString KnownHostName(const QString & HostName, int PortNumber);

  // Windows paths (C:\..., \\server\share, %USERPROFILE%\...) in local
  // file/directory fields of imported sites
  bool IsWindowsPath(const QString & Path);
  bool HasWindowsPaths(const SessionData & Data);
  // Maps the user profile to the home folder; a key file that does not exist
  // there is looked up by name in ~/.ssh; paths that cannot be mapped are
  // cleared. Returns true when anything changed.
  bool RemapWindowsPaths(SessionData & Data);
  QString RemapWindowsPath(const QString & Path, bool IsDirectory);
}
