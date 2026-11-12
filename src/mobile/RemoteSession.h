// One connection to a saved site for the touch interface (a core Terminal):
// browse, download, upload, delete, rename, new folder, with the progress of
// transfers and Cancel. Downloads go to a folder of the phone (the phone
// pane's) or the app's download folder; uploads come from local paths (the
// document picker copies content:// files there first).
#pragma once

#include "core/RemoteFile.h"
#include "core/SessionData.h"
#include "core/Terminal.h"
#include "mobile/FileListModel.h"
#include "mobile/MobileUserInterface.h"
#include "mobile/TerminalSession.h"

#include <QDateTime>
#include <QPointer>
#include <QVariantMap>
#include <QTimer>

class FileFind;
struct FoundFile;
class MobileSettings;
class MobileQueue;

class RemoteSession : public QObject
{
  Q_OBJECT
  Q_PROPERTY(bool connected READ connected NOTIFY stateChanged)
  Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
  // opening the connection (busy, not yet connected)
  Q_PROPERTY(bool connecting READ connecting NOTIFY stateChanged)
  Q_PROPERTY(QString site READ site NOTIFY stateChanged)
  Q_PROPERTY(QString directory READ directory NOTIFY directoryChanged)
  Q_PROPERTY(QString status READ status NOTIFY statusChanged)
  Q_PROPERTY(QString downloadFolder READ downloadFolder CONSTANT)
  Q_PROPERTY(FileListModel * files READ files CONSTANT)
  // the server runs shell commands (console, archives, extract, key install):
  // SFTP and SCP; FTP's "commands" are FTP commands
  Q_PROPERTY(bool canRunCommands READ canRunCommands NOTIFY stateChanged)
  // the interactive shell of this session (null until openTerminal)
  Q_PROPERTY(TerminalSession * terminal READ terminal NOTIFY terminalChanged)
  // a transfer runs: 0..1, or -1 while the size is not known yet
  Q_PROPERTY(bool transferring READ transferring NOTIFY progressChanged)
  Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
  // "12.5 MB of 80 MB · 4.2 MB/s" and the current file
  Q_PROPERTY(QString progressText READ progressText NOTIFY progressChanged)
  Q_PROPERTY(QString progressFile READ progressFile NOTIFY progressChanged)

public:
  RemoteSession(MobileUserInterface * UI, MobileSettings * Settings, MobileQueue * Queue, QObject * Parent = nullptr);
  ~RemoteSession() override;

  bool connected() const;
  bool busy() const { return FBusy; }
  bool connecting() const { return FBusy && !connected() && (FTerminal != nullptr); }
  QString site() const { return FSite; }
  QString directory() const { return FDirectory; }
  QString status() const { return FStatus; }
  QString downloadFolder() const;
  FileListModel * files() { return &FFiles; }
  bool canRunCommands() const { return connected() && FData.IsSsh() && FTerminal->CanAnyCommand(); }
  TerminalSession * terminal() const { return FShell; }
  bool transferring() const { return FTransferring; }
  double progress() const { return FProgress; }
  QString progressText() const { return FProgressText; }
  QString progressFile() const { return FProgressFile; }

  Q_INVOKABLE void open(const QString & SiteName);
  Q_INVOKABLE void close();
  // stops what runs: the connecting (closes the session) or the operation
  Q_INVOKABLE void cancel();
  Q_INVOKABLE void cd(const QString & Path);
  Q_INVOKABLE void enter(const QString & Name);   // a folder of the listing, ".." = up
  Q_INVOKABLE void refresh();
  // queued (the transfer queue, on the background connection): into
  // TargetDir (a folder of the phone), or without it into downloadFolder()
  // and then to Download/NixSCP through Android
  Q_INVOKABLE void download(const QStringList & Names, const QString & TargetDir = QString());
  // downloads one file into the cache and emits opened() (QML hands it to
  // another app: Open, or Share when Share is set)
  Q_INVOKABLE void openFile(const QString & Name, bool Share = false);
  // downloads the files into the cache and emits shared()
  Q_INVOKABLE void shareFiles(const QStringList & Names);
  // queued: local paths, file:// URLs or content:// documents (copied into
  // the cache first, under their own names)
  Q_INVOKABLE void upload(const QStringList & Sources);
  Q_INVOKABLE void remove(const QStringList & Names);
  Q_INVOKABLE void rename(const QString & From, const QString & To);
  Q_INVOKABLE void createFolder(const QString & Name);
  // an empty file, then opened for editing (editReady)
  Q_INVOKABLE void createFile(const QString & Name);

  // ---- editing: the file is downloaded and handed over with editReady();
  // changes of the local copy are uploaded back by checkEdits() (when the
  // app is active again, after another app edited it, or to try a failed
  // upload again). The built-in editor keeps the file in memory: a copy
  // exists only while it is transferred, so nothing stays on the phone.
  Q_INVOKABLE void editFile(const QString & Name);
  Q_INVOKABLE void checkEdits();
  // the built-in editor takes over the file: {text, encoding, lineEnding,
  // sample}, {needsEncoding, encodings, text, sample} for a file that is not
  // UTF-8 (asked again with one of the encodings), or {error}
  Q_INVOKABLE QVariantMap readEdit(const QString & LocalPath, const QString & Encoding = QString());
  // only what changed is replaced in the file, in its own encoding and line
  // endings; uploaded, then editSaved()
  Q_INVOKABLE void saveText(const QString & LocalPath, const QString & Text, const QString & Encoding);
  // the built-in editor closed: the file is forgotten
  Q_INVOKABLE void closeEdit(const QString & LocalPath);
  // the file again for an editor whose session ended: editAdopted()
  Q_INVOKABLE void adoptEdit(const QString & RemotePath);
  // to look at a file: {text} or {error}
  Q_INVOKABLE QVariantMap readText(const QString & LocalPath) const;

  // ---- archives: the selection as one .tar.gz / .zip made on the server
  // (tar / zip; a local .zip for servers without commands), downloaded into
  // TargetDir (empty: downloadFolder(), then downloaded()); archived() when done
  Q_INVOKABLE void archive(const QStringList & Names, const QString & Format, const QString & TargetDir);
  // unpacks an archive of the current folder there (tar / unzip on the server)
  Q_INVOKABLE void extract(const QString & Name);
  Q_INVOKABLE bool isArchive(const QString & Name) const;

  // ---- terminal: an interactive shell (SFTP / SCP sites) of Cols x Rows,
  // started in the current folder; an existing one is kept
  Q_INVOKABLE TerminalSession * openTerminal(int Cols, int Rows);

  // ---- properties: the bits of Set are set and those of Unset cleared on
  // each file, the others stay as they are there; empty Owner / Group keep them
  Q_INVOKABLE void setProperties(const QStringList & Names, int Set, int Unset, const QString & Owner, const QString & Group,
    bool Recursive);

  // ---- preview: the file downloaded into the cache, then previewReady();
  // closePreview when it is no longer looked at
  Q_INVOKABLE void previewFile(const QString & Name);
  Q_INVOKABLE void closePreview(const QString & LocalPath);

  // ---- search below the current folder (WinSCP's Find files; Mask:
  // "*.log", or a part of the name): found() in batches, searchFinished()
  Q_INVOKABLE bool findFiles(const QString & Mask);
  Q_INVOKABLE void stopSearch();

  // ---- copy / move on the server: absolute paths into the current folder
  Q_INVOKABLE void paste(const QStringList & Paths, bool Move);
  // absolute paths of names in the current folder
  Q_INVOKABLE QStringList remotePaths(const QStringList & Names) const;

  // ---- the site's recently visited folders (newest first)
  Q_INVOKABLE QStringList recentFolders() const;

  // ---- bookmarks of the site's folders (WinSCP's, in the configuration): [{name, path}]
  Q_INVOKABLE QVariantList bookmarks() const;
  Q_INVOKABLE bool addBookmark(const QString & Name, const QString & Path);
  Q_INVOKABLE void removeBookmark(const QString & Name);

  // appends an OpenSSH public key to the server's ~/.ssh/authorized_keys
  Q_INVOKABLE void installPublicKey(const QString & Key);
  // the shell command for that; it fails when the key did not get there
  static QString InstallKeyCommand(const QString & Line);

  // a transfer of this session runs or waits (closing it stops them)
  Q_INVOKABLE bool hasTransfers() const;

  // Deletes the session once its connections are gone: their callbacks
  // still run after close()
  void DeleteWhenClosed();
  // Connections and shells of closed sessions that are still ending
  static int Closing();
  // why a note item (SessionData::NoteOnly) does not connect
  static QString NoteMessage(const QString & SiteName);

signals:
  void stateChanged();
  // the connection is open (the first time)
  void established();
  // the session ended: closed, failed to open or lost (SessionManager removes it)
  void closed();
  void directoryChanged();
  void statusChanged();
  void progressChanged();
  // an operation failed (message and details for the user)
  void error(const QString & message, const QString & details);
  // a download finished: the local files
  void downloaded(const QStringList & paths);
  // a file of openFile() is ready
  void opened(const QString & path, bool share);
  void shared(const QStringList & paths);
  // a download into a folder of the phone finished (count of files)
  void copied(const QString & targetDir, int count);
  // an upload finished (count of files)
  void uploaded(int count);
  void editReady(const QString & localPath, const QString & remotePath);
  // a save of the built-in editor reached the server, or did not
  void editSaved(const QString & localPath, bool ok);
  void editAdopted(const QString & localPath, const QString & remotePath);
  // the session ended with changes another app made and that were not
  // uploaded: kept in localPath until the next start
  void editUnsent(const QString & localPath);
  void archived(const QString & path);
  void terminalChanged();
  void previewReady(const QString & localPath, const QString & name);
  // search results: [{name, directory, path, isDir, size}]
  void found(const QVariantList & files);
  void searchFinished(bool aborted, const QString & error);
  // a short message for the user (toast)
  void notice(const QString & message);

private:
  struct Edit
  {
    QString Local;
    QString RemoteDir;
    // the file as uploaded last (another app's editor)
    QDateTime Modified;
    qint64 Size = 0;
    // the built-in editor: the file's bytes and those of a save on its way
    bool BuiltIn = false;
    QByteArray Original;
    // the last save, and what the upload on its way has of it: a save during
    // an upload changes Unsent, which then goes up when that one is done
    QByteArray Unsent;
    QByteArray Sending;
    bool HasUnsent = false;
    // the last upload failed: tried again by checkEdits or the next save
    bool Failed = false;
    bool Closed = false;
  };

  struct Thumb
  {
    QString Name;
    QString Remote;
    QString Cache;
  };

  MobileUserInterface * FUI;
  // the questions of this session's connections
  std::shared_ptr<MobileUserInterface::Asker> FAsker;
  MobileSettings * FSettings;
  MobileQueue * FQueue;
  // the background connection: the queue's transfers, then thumbnails
  QPointer<Terminal> FBack;
  bool FBackOpening = false;
  bool FBackThumb = false;     // a thumbnail is being fetched
  int FBackItem = 0;           // the queue item running
  QTimer FBackTimer;
  QList<Thumb> FThumbs;
  QString FThumbDirectory;
  int FUploadCounter = 0;
  QPointer<FileFind> FFind;
  SessionData FData;
  // as opened (the phone's settings, known_hosts): for the terminal's connection
  SessionData FOpenData;
  QPointer<TerminalSession> FShell;
  // its own folders in the cache (MobileCache)
  int FCacheId;
  QList<Edit> FEdits;
  int FEditCounter = 0;
  // the edit being uploaded
  QString FUploadingEdit;
  // connections that are ending, and what comes after them
  int FClosing = 0;
  bool FDeleteClosed = false;
  // an upload ended while the folder was being read: listed again afterwards
  bool FStale = false;
  QPointer<Terminal> FTerminal;
  FileListModel FFiles;
  QTimer FProgressTimer;
  bool FTransferring = false;
  double FProgress = -1;
  QString FProgressText;
  QString FProgressFile;
  QString FSite;
  QString FDirectory;
  QString FStatus;
  bool FBusy = false;

  void SetBusy(bool Busy, const QString & Status = QString());
  void Load(const QString & Path);
  QString RemotePath(const QString & Name) const;
  bool Failed(const OperationResult & Result);
  void StartTransfer(const QString & Status);
  void StopTransfer();
  // Overwrite: without asking (edited files)
  void UploadTo(const QStringList & LocalPaths, const QString & RemoteDir, std::function<void(bool)> Done,
    bool Overwrite = false);
  // the command in the current folder
  QString InDirectory(const QString & Command) const;
  // runs a command quietly: Done with the exit code (-1 failed) and the output
  void Command(const QString & Command, const QString & Status,
    std::function<void(int, const QStringList &)> Done);
  void ZipLocally(const QStringList & Names, const QString & ArchiveName, const QString & TargetDir);
  void CopiesReady(const QStringList & LocalPaths, const QString & Bad, const QString & CacheDir, const QString & Target);
  // the end of a connection (it may still be working)
  void Dispose(Terminal * Term);
  // the session's questions stay valid as long as the connection lives
  void KeepAsker(QObject * Connection);
  Edit * FindEdit(const QString & Local);
  void FetchEdit(const QString & Name, bool Adopt);
  // uploads the next edit that has something to send
  void SendEdits();
  bool WriteEdit(const Edit & E);
  void EditUploaded(const QString & Local, bool Ok, const QDateTime & Modified, qint64 Size);
  // the queue and thumbnails on the background connection
  void RunNext();
  void WithBack(std::function<void(bool)> Ready);
  void RunItem(int Id);
  void NextThumbnail();
  void QueueThumbnails(const RemoteFileList & Files);
  void DropBack();
  void UpdateProgress();
  // downloads Names into Target (emptied first with Clean), then Done with
  // the local files; Exact: as binary, whatever the transfer settings are
  void Fetch(const QStringList & Names, const QString & Target, bool Clean, const QString & Status,
    std::function<void(const QStringList &)> Done, bool Exact = false);
};
