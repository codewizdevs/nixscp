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
#include "mobile/TerminalSession.h"

#include <QDateTime>
#include <QPointer>
#include <QVariantMap>
#include <QTimer>

class FileFind;
struct FoundFile;
class MobileSettings;
class MobileUserInterface;
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
  // the server runs commands (console, archives, extract, key install): SFTP and SCP
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
  bool canRunCommands() const { return connected() && FTerminal->CanAnyCommand(); }
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
  // app is active again, after another app edited it) or saveText()
  Q_INVOKABLE void editFile(const QString & Name);
  Q_INVOKABLE void checkEdits();
  // the built-in editor: {text} or {error}
  Q_INVOKABLE QVariantMap readText(const QString & LocalPath) const;
  // writes the text and uploads the file
  Q_INVOKABLE void saveText(const QString & LocalPath, const QString & Text);

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

  // ---- properties: Mode < 0 keeps the rights; empty Owner / Group keep them
  Q_INVOKABLE void setProperties(const QStringList & Names, int Mode, const QString & Owner, const QString & Group,
    bool Recursive);

  // ---- preview: the file downloaded into the cache, then previewReady()
  Q_INVOKABLE void previewFile(const QString & Name);

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
    QDateTime Modified;
    qint64 Size = 0;
  };

  struct Thumb
  {
    QString Name;
    QString Remote;
    QString Cache;
  };

  MobileUserInterface * FUI;
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
  // the decrypted copy of the session's key (removed on close)
  QString FTemporaryKey;
  QList<Edit> FEdits;
  int FEditCounter = 0;
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
  // the queue and thumbnails on the background connection
  void RunNext();
  void WithBack(std::function<void(bool)> Ready);
  void RunItem(int Id);
  void NextThumbnail();
  void QueueThumbnails(const RemoteFileList & Files);
  void DropBack();
  void UpdateProgress();
  // downloads Names into Target (emptied first with Clean), then Done with the local files
  void Fetch(const QStringList & Names, const QString & Target, bool Clean, const QString & Status,
    std::function<void(const QStringList &)> Done);
};
