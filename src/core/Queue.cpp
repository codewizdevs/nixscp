// Port of WinSCP source/core/Queue.cpp (TTerminalQueue, TTerminalItem,
// TTransferQueueItem, TParallelTransferQueueItem, TRemoteDeleteQueueItem,
// TLocalDeleteQueueItem, TBackgroundTerminal) and the parallel transfer part
// of source/core/Terminal.cpp (TParallelOperation, CopyToParallel,
// CheckParallelFileTransfer).
//
// As in WinSCP, items are kept in one list: started items (done, failed,
// connecting, processing, suspended) first, pending items after them, in the
// order they will be processed. Up to TransfersLimit worker connections
// (separate Terminals, each with its own thread) are opened lazily and reused
// for subsequent items; "Execute Now" (Resume of a pending item) may open one
// temporary connection over the limit (TTerminalQueue::FTemporaryTerminals).
//
// Parallel transfers (TParallelOperation): an eligible item first collects
// its files on one connection ("Listing..."); then every connection working
// on the item takes the next file (or the next segment of a large file) until
// none is left. More connections join while there are idle ones, or (up to
// the limit) when the transfer is expected to take at least
// ParallelDurationThreshold more, at most one every 5 seconds. A joined
// connection leaves after its current file when another item is waiting
// (TTerminalQueue::ContinueParallelOperation).
//
// User actions (TTerminalItem::WaitForUserAction): queries, prompts and host
// key confirmations of a background connection block its thread; the item
// shows "Query"/"Prompt"/"Error" until ProcessUserAction shows the dialog on
// the GUI thread.
//
// All methods run on the GUI thread. Terminal callbacks are never handled
// inside the Terminal's own callback invocation: they are re-posted to the
// queue object first, so a Terminal can be deleted and a receiver of
// ItemFinished/Changed may even delete the queue.
#include "core/Queue.h"
#include "core/Platform.h"

#include "core/Configuration.h"
#include "core/IniStorage.h"
#include "core/LocalFileOperations.h"
#include "core/ScpFileSystem.h"
#include "core/SftpFileSystem.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QMutex>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QWaitCondition>

#include <libssh/sftp.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <list>
#include <memory>
#include <sys/stat.h>
#include <vector>

namespace
{

// TTerminalQueue::ProcessEvent waits at most a second, so done items expire,
// and TTransferQueueItem::ProgressUpdated adds parallel connections
const int HousekeepingIntervalMs = 1000;
// TTransferQueueItem::ProgressUpdated: at most one new connection per 5 s
const int ParallelAddIntervalMs = 5 * 1000;
// Consecutive lost connections of a parallel transfer without any file done
const int MaxParallelFatalFailures = 3;
// Defaults of hidden settings (core/Configuration.cpp)
const int DefaultSessionReopenBackground = 2000;
const int DefaultParallelDurationThreshold = 10;

// ---------------------------------------------------------------------------
// User action of a background connection (TUserAction): created on the worker
// thread, executed on the GUI thread; results are stored here, never in the
// worker's stack, so the worker can give up waiting at any time
struct UserAction
{
  enum Type { atQuery, atPrompt, atHostKey };

  Type Kind = atQuery;
  QueueItemInfo::Status Status = QueueItemInfo::qsQuery;
  QString Text;

  // atQuery
  QString Query;
  QString MoreMessages;
  unsigned int Answers = 0;
  QueryUserParams Params;
  bool Ex = false;
  unsigned int Answer = qaCancel;
  // atPrompt
  UserInterface::PromptKind PromptKind = UserInterface::pkPassword;
  QString Name;
  QString Instructions;
  QList<Prompt> Prompts;
  bool PromptResult = false;
  // atHostKey
  QString Host;
  int Port = 0;
  QString KeyType;
  QString FingerprintSHA256;
  QString FingerprintMD5;
  UserInterface::HostKeyState KeyState = UserInterface::HostKeyUnknown;
  UserInterface::HostKeyAnswer KeyAnswer = UserInterface::HostKeyCancel;

  bool Executing = false;   // GUI thread only

  QMutex Mutex;
  QWaitCondition Cond;
  bool Done = false;
  bool Cancelled = false;

  // GUI thread (or the worker thread when actions are not deferred)
  void Execute(UserInterface & UI)
  {
    switch (Kind)
    {
      case atQuery:
        {
          unsigned int Result = Ex ? UI.QueryUserEx(Query, MoreMessages, Answers, Params) : UI.QueryUser(Query, MoreMessages, Answers);
          QMutexLocker Lock(&Mutex);
          Answer = Result;
        }
        break;
      case atPrompt:
        {
          QList<Prompt> P = Prompts;
          bool Result = UI.PromptUser(PromptKind, Name, Instructions, P);
          QMutexLocker Lock(&Mutex);
          Prompts = P;
          PromptResult = Result;
        }
        break;
      case atHostKey:
        {
          UserInterface::HostKeyAnswer Result = UI.VerifyHostKey(Host, Port, KeyType, FingerprintSHA256, FingerprintMD5, KeyState);
          QMutexLocker Lock(&Mutex);
          KeyAnswer = Result;
        }
        break;
    }
  }

  void Complete(bool Cancel)
  {
    QMutexLocker Lock(&Mutex);
    if (!Done)
    {
      Done = true;
      Cancelled = Cancel;
      Cond.wakeAll();
    }
  }
};
using UserActionPtr = std::shared_ptr<UserAction>;

// Answer used when a query is cancelled (item deleted, queue closing)
static unsigned int CancelAnswer(unsigned int Answers)
{
  const unsigned int Order[] = {qaCancel, qaAbort, qaNo, qaSkip, qaOK};
  for (unsigned int Answer : Order)
  {
    if ((Answers & Answer) != 0)
    {
      return Answer;
    }
  }
  return qaCancel;
}

// TTerminal::IsPasswordOrPassphrasePrompt
static bool IsPasswordOrPassphrasePrompt(UserInterface::PromptKind Kind, const QList<Prompt> & Prompts)
{
  return (Prompts.size() == 1) && !Prompts[0].Echo &&
    ((Kind == UserInterface::pkPassword) || (Kind == UserInterface::pkPassphrase) || (Kind == UserInterface::pkKeybInteractive));
}

// Item state shared with the worker threads of its connections
struct ItemShared
{
  QMutex Mutex;
  unsigned int OverwriteAll = 0;   // Yes to All / No to All answered on any connection of the item
};
using ItemSharedPtr = std::shared_ptr<ItemShared>;

// Queue state shared with the worker threads
struct QueueShared
{
  QMutex Mutex;
  QString Password;        // remembered password (TTerminal::FRememberedPassword)
  QString Passphrase;      // remembered passphrase
  QString DataPassword;    // password of the session data, tried by the connection itself
  bool Deferred = true;
  bool Remember = true;    // Configuration::SessionRememberPassword

  QString RememberedFor(UserInterface::PromptKind Kind)
  {
    QMutexLocker Lock(&Mutex);
    if (Kind == UserInterface::pkPassphrase)
    {
      return Passphrase;
    }
    // the connection tried the session password already; do not try it twice
    return (Password != DataPassword) ? Password : QString();
  }

  void Store(UserInterface::PromptKind Kind, const QString & Value)
  {
    QMutexLocker Lock(&Mutex);
    if (!Remember)
    {
      return;
    }
    if (Kind == UserInterface::pkPassphrase)
    {
      Passphrase = Value;
    }
    else
    {
      Password = Value;
    }
  }
};
using QueueSharedPtr = std::shared_ptr<QueueShared>;

// ---------------------------------------------------------------------------
// User interface of one background connection (TTerminalItem::TerminalQueryUser,
// TerminalPromptUser; TBackgroundTerminal shows no banners). Called on the
// worker thread of its Terminal; owned by the Terminal (QObject child), so it
// outlives the Terminal's thread.
class WorkerUI : public QObject, public UserInterface
{
public:
  using PostFunction = std::function<void(const UserActionPtr &)>;

  WorkerUI(UserInterface * RealUI, const QueueSharedPtr & Shared, PostFunction Post) :
    FRealUI(RealUI), FShared(Shared), FPost(std::move(Post))
  {
  }

  // GUI thread: the item served by the connection
  void SetItem(const ItemSharedPtr & Item)
  {
    QMutexLocker Lock(&FMutex);
    FItem = Item;
  }

  // GUI thread: no more user actions, pending ones are cancelled
  void Close()
  {
    QList<UserActionPtr> Pending;
    {
      QMutexLocker Lock(&FMutex);
      FClosing = true;
      Pending = FPending;
      FPending.clear();
    }
    for (const UserActionPtr & Action : Pending)
    {
      Action->Complete(true);
    }
  }

  HostKeyAnswer VerifyHostKey(const QString & Host, int Port, const QString & KeyType,
    const QString & FingerprintSHA256, const QString & FingerprintMD5, HostKeyState State) override
  {
    auto Action = std::make_shared<UserAction>();
    Action->Kind = UserAction::atHostKey;
    Action->Status = QueueItemInfo::qsQuery;
    Action->Host = Host;
    Action->Port = Port;
    Action->KeyType = KeyType;
    Action->FingerprintSHA256 = FingerprintSHA256;
    Action->FingerprintMD5 = FingerprintMD5;
    Action->KeyState = State;
    Action->Text = (State == HostKeyChanged) ?
      QStringLiteral("WARNING - POTENTIAL SECURITY BREACH! The server's host key does not match.") :
      QStringLiteral("Continue connecting to an unknown server and add its host key to a cache?");
    if (!Request(Action))
    {
      return HostKeyCancel;
    }
    QMutexLocker Lock(&Action->Mutex);
    return Action->KeyAnswer;
  }

  bool PromptUser(PromptKind Kind, const QString & Name, const QString & Instructions, QList<Prompt> & Prompts) override
  {
    bool PasswordPrompt = IsPasswordOrPassphrasePrompt(Kind, Prompts);
    // TTerminal::DoPromptUser: the first password prompt of a secondary
    // connection is answered with the password remembered in the session
    if (PasswordPrompt && !FPasswordTried)
    {
      FPasswordTried = true;
      QString Remembered = FShared->RememberedFor(Kind);
      if (!Remembered.isEmpty())
      {
        Prompts[0].Result = Remembered;
        return true;
      }
    }
    auto Action = std::make_shared<UserAction>();
    Action->Kind = UserAction::atPrompt;
    Action->Status = QueueItemInfo::qsPrompt;
    Action->PromptKind = Kind;
    Action->Name = Name;
    Action->Instructions = Instructions;
    Action->Prompts = Prompts;
    Action->Text = Prompts.isEmpty() ? Instructions : Prompts.first().Text;
    if (!Request(Action))
    {
      return false;
    }
    QMutexLocker Lock(&Action->Mutex);
    if (Action->PromptResult)
    {
      Prompts = Action->Prompts;
      if (PasswordPrompt && !Prompts.isEmpty())
      {
        FShared->Store(Kind, Prompts[0].Result);
      }
    }
    return Action->PromptResult;
  }

  void DisplayBanner(const QString &, const QString &) override
  {
    // Background connections do not show the banner (TBackgroundTerminal has no OnDisplayBanner)
  }

  unsigned int QueryUser(const QString & Query, const QString & MoreMessages, unsigned int Answers) override
  {
    return DoQueryUser(Query, MoreMessages, Answers, QueryUserParams(), false);
  }

  unsigned int QueryUserEx(const QString & Query, const QString & MoreMessages, unsigned int Answers,
    const QueryUserParams & Params) override
  {
    return DoQueryUser(Query, MoreMessages, Answers, Params, true);
  }

private:
  UserInterface * FRealUI;
  QueueSharedPtr FShared;
  PostFunction FPost;
  QMutex FMutex;
  bool FClosing = false;
  ItemSharedPtr FItem;
  QList<UserActionPtr> FPending;
  bool FPasswordTried = false;   // worker thread only

  unsigned int DoQueryUser(const QString & Query, const QString & MoreMessages, unsigned int Answers,
    const QueryUserParams & Params, bool Ex)
  {
    // Batch overwrite answer (OperationProgress->BatchOverwrite of the main
    // operation) is shared by all connections of a parallel transfer
    bool Batch = ((Answers & (qaYesToAll | qaNoToAll)) != 0);
    ItemSharedPtr Item;
    {
      QMutexLocker Lock(&FMutex);
      Item = FItem;
    }
    if (Batch && Item)
    {
      QMutexLocker Lock(&Item->Mutex);
      if (Item->OverwriteAll != 0)
      {
        return Item->OverwriteAll;
      }
    }
    auto Action = std::make_shared<UserAction>();
    Action->Kind = UserAction::atQuery;
    // TTerminalItem::TerminalQueryUser: an error query is presented as "Error"
    bool Error = (Params.Type == qtError) || ((Answers & (qaRetry | qaSkip)) != 0);
    Action->Status = Error ? QueueItemInfo::qsError : QueueItemInfo::qsQuery;
    Action->Query = Query;
    Action->MoreMessages = MoreMessages;
    Action->Answers = Answers;
    Action->Params = Params;
    Action->Ex = Ex;
    Action->Answer = CancelAnswer(Answers);
    Action->Text = Query;
    if (!Request(Action))
    {
      return CancelAnswer(Answers);
    }
    unsigned int Answer;
    {
      QMutexLocker Lock(&Action->Mutex);
      Answer = Action->Answer;
    }
    if (Batch && Item && ((Answer == qaYesToAll) || (Answer == qaNoToAll)))
    {
      QMutexLocker Lock(&Item->Mutex);
      Item->OverwriteAll = Answer;
    }
    return Answer;
  }

  // Worker thread: waits for the user action; false when cancelled
  bool Request(const UserActionPtr & Action)
  {
    bool Deferred;
    {
      QMutexLocker Lock(&FShared->Mutex);
      Deferred = FShared->Deferred;
    }
    {
      QMutexLocker Lock(&FMutex);
      if (FClosing)
      {
        return false;
      }
      if (Deferred)
      {
        FPending.append(Action);
        // posted under the lock: Close() cannot run in between, so the queue still exists
        FPost(Action);
      }
    }
    if (!Deferred)
    {
      Action->Execute(*FRealUI);
      Action->Complete(false);
      return true;
    }
    bool Cancelled;
    {
      QMutexLocker Lock(&Action->Mutex);
      while (!Action->Done)
      {
        Action->Cond.wait(&Action->Mutex);
      }
      Cancelled = Action->Cancelled;
    }
    {
      QMutexLocker Lock(&FMutex);
      FPending.removeAll(Action);
    }
    return !Cancelled;
  }
};

// ---------------------------------------------------------------------------
// Parallel transfer (TParallelOperation)

// One piece of work of a parallel transfer
struct Unit
{
  enum Type { ukFile, ukDir, ukPart };
  Type Kind = ukFile;
  QString Source;         // file / directory / remote file of the segment
  QString Target;         // ukFile: target directory; ukDir: directory to create; ukPart: part file
  qint64 Size = 0;
  unsigned int Mode = 0;  // ukDir
  int Parent = -1;        // unit (ukDir) that must be done first
  enum State { usPending, usRunning, usDone };
  State Status = usPending;
  int Index = 0;          // ukPart: segment number
  qint64 Offset = 0;      // ukPart
  qint64 Length = -1;     // ukPart, -1 = until the end
};

// Segmented download of one large file, shared with the worker threads
// (TParallelOperation::Done: parts are merged in order as they complete)
struct SegmentState
{
  QMutex Mutex;
  QString RemotePath;
  QString TargetPath;
  qint64 Size = 0;
  QDateTime Modification;
  bool PreserveTime = true;
  std::vector<bool> PartDone;
  int Merged = 0;
  bool Merging = false;
};
using SegmentStatePtr = std::shared_ptr<SegmentState>;

// GetPartPrefix: "<file>.filepart.<n>"
static QString PartFileName(const QString & TargetPath, int Index)
{
  return TargetPath + PartialExt + QLatin1Char('.') + QString::number(Index);
}

struct ParallelState
{
  bool Scanned = false;
  bool Segmented = false;
  std::vector<Unit> Units;
  size_t FirstPending = 0;
  int Running = 0;
  int DoneCount = 0;
  qint64 TotalSize = 0;
  int FileCount = 0;
  qint64 DoneTransferred = 0;
  int DoneFiles = 0;
  QElapsedTimer Elapsed;          // since the transfer started
  QElapsedTimer LastClientAdded;  // FLastParallelOperationAdded
  int FatalFailures = 0;
  bool Failing = false;
  OperationResult Failure;
  SegmentStatePtr Segment;
};

// Result of the listing job (TTerminal::CollectFiles of CopyToRemote/CopyToLocal)
struct ScanEntry
{
  QString Source;
  QString Target;
  bool Directory = false;
  qint64 Size = 0;
  unsigned int Mode = 0;
  int Parent = -1;        // index of the parent directory entry, -1 = top level
};

struct ScanResult
{
  bool Parallel = true;   // false: protocol cannot transfer in parallel
  bool Skip = false;      // nothing to transfer (overwrite declined, not newer)
  bool Segment = false;   // single large file to download in segments
  QDateTime Modification;
  std::vector<ScanEntry> Entries;
};

static FileMasks::Params MaskParams(qint64 Size, const QDateTime & Modification)
{
  FileMasks::Params Result;
  Result.Size = Size;
  Result.Modification = Modification;
  return Result;
}

static bool AnyFileFrom(const std::vector<ScanEntry> & Entries, size_t Index)
{
  for (size_t I = Index; I < Entries.size(); I++)
  {
    if (!Entries[I].Directory)
    {
      return true;
    }
  }
  return false;
}

// Worker thread (Terminal::ScanRemote)
static void ScanRemote(FileSystem & Fs, FileOperationProgress & Progress, const QString & Path, const RemoteFile & File,
  const QString & Target, int Parent, const CopyParam & Param, std::vector<ScanEntry> & Entries)
{
  Progress.CheckCancel();
  if (!Param.AllowTransfer(Path, false, File.IsDirectory, MaskParams(File.Size, File.Modification), File.IsHidden))
  {
    return;
  }
  size_t Index = Entries.size();
  ScanEntry Entry;
  Entry.Source = Path;
  Entry.Target = Target;
  Entry.Directory = File.IsDirectory;
  Entry.Size = File.IsDirectory ? 0 : File.Size;
  Entry.Mode = File.Permissions;
  Entry.Parent = Parent;
  Entries.push_back(Entry);
  if (File.IsDirectory)
  {
    Progress.SetFile(Path, 0);
    for (const RemoteFile & Child : Fs.ReadDirectory(Path))
    {
      if (Child.IsParentDirectory || !IsValidListingName(Child.FileName))
      {
        continue;
      }
      ScanRemote(Fs, Progress, UnixPath::Combine(Path, Child.FileName), Child,
        QDir(Target).filePath(CopyParam::ValidLocalFileName(Param.ChangeFileName(Child.FileName))), static_cast<int>(Index), Param, Entries);
    }
    if (Param.ExcludeEmptyDirectories && !AnyFileFrom(Entries, Index + 1))
    {
      Entries.resize(Index);
    }
  }
  else
  {
    Progress.AddTotalSize(File.Size);
  }
}

// Worker thread (Terminal::ScanLocal)
static void ScanLocal(FileOperationProgress & Progress, const QString & Path, const QString & Target, int Parent,
  const CopyParam & Param, QSet<QString> & Visited, std::vector<ScanEntry> & Entries)
{
  Progress.CheckCancel();
  QFileInfo Info(Path);
  if (!Info.exists())
  {
    throw ExtException(QStringLiteral("File or folder '%1' does not exist.").arg(Path));
  }
  ScanEntry Entry;
  Entry.Source = Path;
  Entry.Target = Target;
  Entry.Directory = Info.isDir();
  Entry.Size = Entry.Directory ? 0 : Info.size();
  Entry.Parent = Parent;
  if (!Param.AllowTransfer(Path, true, Entry.Directory, MaskParams(Entry.Size, Info.lastModified()),
        Info.fileName().startsWith(QLatin1Char('.'))))
  {
    return;
  }
  struct stat St;
  if (stat(QFile::encodeName(Path).constData(), &St) == 0)
  {
    Entry.Mode = St.st_mode & 07777;
  }
  size_t Index = Entries.size();
  Entries.push_back(Entry);
  if (Entry.Directory)
  {
    QString Canonical = Info.canonicalFilePath();
    if (Visited.contains(Canonical))
    {
      return; // symlink loop
    }
    Visited.insert(Canonical);
    Progress.SetFile(Path, 0);
    QDir Dir(Path);
    const QFileInfoList Children = Dir.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo & Child : Children)
    {
      ScanLocal(Progress, Child.filePath(), UnixPath::Combine(Target, CopyParam::RestoreChars(Param.ChangeFileName(Child.fileName()))),
        static_cast<int>(Index), Param, Visited, Entries);
    }
    if (Param.ExcludeEmptyDirectories && !AnyFileFrom(Entries, Index + 1))
    {
      Entries.resize(Index);
    }
  }
  else
  {
    Progress.AddTotalSize(Entry.Size);
  }
}

static void RenameLocalForce(const QString & From, const QString & To)
{
  // rename() replaces an existing target atomically (DoRenameLocalFileForce)
  if (rename(QFile::encodeName(From).constData(), QFile::encodeName(To).constData()) != 0)
  {
    throw ExtException(QStringLiteral("Cannot rename file '%1' to '%2'.").arg(QFileInfo(From).fileName(), QFileInfo(To).fileName()),
      QString::fromLocal8Bit(strerror(errno)));
  }
}

static void SetLocalModificationTime(const QString & Path, const QDateTime & Time)
{
  Platform::SetFileTimes(Path, Time.toSecsSinceEpoch());
}

// Worker thread: appends finished parts to "<file>.filepart" in order and
// renames it to the target when complete (TParallelOperation::Done)
static void MergeParts(const SegmentStatePtr & Seg, int Index)
{
  {
    QMutexLocker Lock(&Seg->Mutex);
    Seg->PartDone[static_cast<size_t>(Index)] = true;
    if (Seg->Merging)
    {
      return;
    }
    // Once we obtain "merging" semaphore, we won't leave until everything is merged
    Seg->Merging = true;
  }
  QString Partial = Seg->TargetPath + PartialExt;
  try
  {
    for (;;)
    {
      int Merge;
      int Count;
      {
        QMutexLocker Lock(&Seg->Mutex);
        Count = static_cast<int>(Seg->PartDone.size());
        if ((Seg->Merged >= Count) || !Seg->PartDone[static_cast<size_t>(Seg->Merged)])
        {
          Seg->Merging = false;
          if (Seg->Merged < Count)
          {
            return;
          }
          break;
        }
        Merge = Seg->Merged;
      }
      QString PartName = PartFileName(Seg->TargetPath, Merge);
      if (Merge == 0)
      {
        // "Renaming part 0 of ..."
        RenameLocalForce(PartName, Partial);
      }
      else
      {
        // "Appending part N of ..."
        QFile Source(PartName);
        QFile Dest(Partial);
        if (!Source.open(QIODevice::ReadOnly))
        {
          throw ExtException(QStringLiteral("Cannot open file '%1'.").arg(PartName), Source.errorString());
        }
        if (!Dest.open(QIODevice::WriteOnly | QIODevice::Append))
        {
          throw ExtException(QStringLiteral("Can't write to file '%1'.").arg(Partial), Dest.errorString());
        }
        QByteArray Buffer(1024 * 1024, Qt::Uninitialized);
        for (;;)
        {
          qint64 Read = Source.read(Buffer.data(), Buffer.size());
          if (Read < 0)
          {
            throw ExtException(QStringLiteral("Cannot read file '%1'.").arg(PartName), Source.errorString());
          }
          if (Read == 0)
          {
            break;
          }
          if (Dest.write(Buffer.constData(), Read) != Read)
          {
            throw ExtException(QStringLiteral("Can't write to file '%1'.").arg(Partial), Dest.errorString());
          }
        }
        if (!Dest.flush())
        {
          throw ExtException(QStringLiteral("Can't write to file '%1'.").arg(Partial), Dest.errorString());
        }
        Source.close();
        Dest.close();
        QFile::remove(PartName);
      }
      QMutexLocker Lock(&Seg->Mutex);
      Seg->Merged++;
    }
  }
  catch (...)
  {
    QMutexLocker Lock(&Seg->Mutex);
    Seg->Merging = false;
    throw;
  }
  // "Renaming completed ..."
  RenameLocalForce(Partial, Seg->TargetPath);
  if (Seg->PreserveTime && Seg->Modification.isValid())
  {
    SetLocalModificationTime(Seg->TargetPath, Seg->Modification);
  }
}

// Worker thread: downloads one segment of a file into its part file over a
// separate SFTP channel of the connection (CustomCopyParam->PartOffset/PartSize)
static void DownloadPart(FileSystem & Fs, FileOperationProgress & Progress, const SegmentStatePtr & Seg,
  int Index, qint64 Offset, qint64 Length)
{
  SftpFileSystem * Sftp = dynamic_cast<SftpFileSystem *>(&Fs);
  if (Sftp == nullptr)
  {
    throw ExtException(QStringLiteral("Operation not supported."));
  }
  ssh_session Session = Sftp->Shell().Session();
  auto Lost = [Session]()
  {
    throw FatalException(QStringLiteral("Connection has been unexpectedly closed."),
      (Session != nullptr) ? QString::fromUtf8(ssh_get_error(Session)) : QString());
  };
  if ((Session == nullptr) || !ssh_is_connected(Session))
  {
    Lost();
  }
  QString RemotePath = Seg->RemotePath;
  sftp_session S = sftp_new(Session);
  if (S == nullptr)
  {
    Lost();
  }
  std::unique_ptr<sftp_session_struct, void (*)(sftp_session)> SGuard(S, sftp_free);
  if (sftp_init(S) != SSH_OK)
  {
    if (!ssh_is_connected(Session))
    {
      Lost();
    }
    throw ExtException(QStringLiteral("Cannot initialize SFTP protocol. Is the host running an SFTP server?"),
      QString::fromUtf8(ssh_get_error(Session)));
  }
  auto SftpError = [&](const QString & Message)
  {
    if (!ssh_is_connected(Session))
    {
      Lost();
    }
    throw ExtException(Message, QString::fromUtf8(ssh_get_error(Session)));
  };
  sftp_file File = sftp_open(S, RemotePath.toUtf8().constData(), O_RDONLY, 0);
  if (File == nullptr)
  {
    SftpError(QStringLiteral("Cannot open remote file '%1'.").arg(RemotePath));
  }
  QString PartName = PartFileName(Seg->TargetPath, Index);
  QFile Local(PartName);
  if (!Local.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    sftp_close(File);
    throw ExtException(QStringLiteral("Can't create file '%1'.").arg(PartName), Local.errorString());
  }
  if ((Offset > 0) && (sftp_seek64(File, static_cast<quint64>(Offset)) != 0))
  {
    sftp_close(File);
    SftpError(QStringLiteral("Cannot read remote file '%1'.").arg(RemotePath));
  }

  sftp_limits_t Limits = sftp_limits(S);
  size_t Chunk = (Limits != nullptr) ? static_cast<size_t>(Limits->max_read_length) : 32768;
  if (Limits != nullptr)
  {
    sftp_limits_free(Limits);
  }
  Chunk = std::max<size_t>(1024, std::min<size_t>(Chunk, 256 * 1024));
  const size_t Depth = 32;

  struct Request
  {
    sftp_aio Aio;
    size_t Len;
  };
  std::deque<Request> InFlight;
  QByteArray Buffer(static_cast<qsizetype>(Chunk), Qt::Uninitialized);
  // Until the end: read up to the known size, then probe for EOF
  qint64 End = (Length >= 0) ? (Offset + Length) : Seg->Size;
  qint64 Requested = Offset;
  qint64 Received = Offset;
  auto Drain = [&]()
  {
    while (!InFlight.empty())
    {
      Request Req = InFlight.front();
      InFlight.pop_front();
      sftp_aio_wait_read(&Req.Aio, Buffer.data(), Req.Len);
      sftp_aio_free(Req.Aio);
    }
  };
  try
  {
    bool Eof = false;
    while (!Eof && ((Length < 0) || (Received < End)))
    {
      Progress.CheckCancel();
      while ((InFlight.size() < Depth) && ((Requested < End) || ((Length < 0) && InFlight.empty())))
      {
        size_t Len = Chunk;
        if (Requested < End)
        {
          Len = static_cast<size_t>(std::min<qint64>(static_cast<qint64>(Chunk), End - Requested));
          // a cancel makes the speed limit pass everything: stop filling the pipeline
          Progress.CheckCancel();
          Len = Progress.AdjustToCPSLimit(static_cast<unsigned long>(Len), InFlight.empty());
          if (Len == 0)
          {
            break;
          }
        }
        sftp_aio Aio = nullptr;
        if (sftp_aio_begin_read(File, Len, &Aio) == SSH_ERROR)
        {
          SftpError(QStringLiteral("Cannot read remote file '%1'.").arg(RemotePath));
        }
        InFlight.push_back(Request{Aio, Len});
        Requested += static_cast<qint64>(Len);
      }
      if (InFlight.empty())
      {
        break;
      }
      Request Req = InFlight.front();
      InFlight.pop_front();
      ssize_t Read = sftp_aio_wait_read(&Req.Aio, Buffer.data(), Req.Len);
      if (Read == SSH_ERROR)
      {
        SftpError(QStringLiteral("Cannot read remote file '%1'.").arg(RemotePath));
      }
      if (Read == 0)
      {
        Eof = true;
        break;
      }
      if (Local.write(Buffer.constData(), Read) != Read)
      {
        throw ExtException(QStringLiteral("Can't write to file '%1'.").arg(PartName), Local.errorString());
      }
      Received += Read;
      Progress.AddTransferred(Read);
      if (static_cast<size_t>(Read) < Req.Len)
      {
        // short read: restart the pipeline at the current offset
        Drain();
        if (sftp_seek64(File, static_cast<quint64>(Received)) != 0)
        {
          SftpError(QStringLiteral("Cannot read remote file '%1'.").arg(RemotePath));
        }
        Requested = Received;
      }
    }
    Drain();
  }
  catch (...)
  {
    try { Drain(); } catch (...) {}
    sftp_close(File);
    Local.close();
    Local.remove();
    throw;
  }
  sftp_close(File);
  if (!Local.flush())
  {
    Local.close();
    Local.remove();
    throw ExtException(QStringLiteral("Can't write to file '%1'.").arg(PartName), Local.errorString());
  }
  Local.close();
  if ((Length >= 0) && (Received - Offset != Length))
  {
    // INCONSISTENT_SIZE
    QFile::remove(PartName);
    throw ExtException(QStringLiteral("Size of the transferred file part '%1' is %2 bytes, but expected %3 bytes.")
      .arg(QFileInfo(PartName).fileName()).arg(Received - Offset).arg(Length));
  }
  Progress.FileFinished();
  MergeParts(Seg, Index);
}

// ---------------------------------------------------------------------------

struct Worker
{
  int Id = 0;
  Terminal * Term = nullptr;
  WorkerUI * UI = nullptr;
  bool Opened = false;       // Open succeeded
  bool Reused = false;       // current item is not the first one on this connection
  int ItemId = 0;            // 0 = idle
  bool Helper = false;       // joined a parallel transfer (TParallelTransferQueueItem)
  int UnitIndex = -1;        // unit of the parallel transfer being processed
};

struct Item
{
  QueueItemInfo Info;
  CopyParam Param;
  bool ToTrash = false;      // ikDelete of local files
  bool Started = false;      // left pending state (incl. finished)
  bool Finished = false;     // qsDone/qsError
  bool Suspended = false;    // user suspended (pending or processing)
  bool Forced = false;       // "Execute Now" or a retry: may exceed the limit
  bool Deleted = false;      // deleted while connecting/processing: waiting for the worker(s)
  bool Parallel = false;     // may use more connections (TTerminal::CanParallel)
  bool ConnectFailed = false;// failed to open the connection: OK in Show Error retries
  bool Reconnecting = false; // waiting to reopen a lost connection
  QElapsedTimer RetryTimer;  // running while Reconnecting
  int RetryDelayMs = 0;
  qint64 PrevTransferred = -1;  // progress of the previous attempt (reconnect only while progressing)
  int PrevFiles = -1;
  std::shared_ptr<ParallelState> PS;
  ItemSharedPtr Shared = std::make_shared<ItemShared>();
  QList<UserActionPtr> Actions;  // pending user actions of the item's connections
  QElapsedTimer DoneTimer;   // since qsDone (QueueKeepDoneItemsFor)
};

}

struct TransferQueue::Private
{
  TransferQueue * Q = nullptr;
  SessionData Data;
  UserInterface * UI = nullptr;
  QueueSharedPtr Shared = std::make_shared<QueueShared>();
  std::list<Item> Items;
  std::list<Worker> Workers;
  QList<QPointer<Terminal>> Discarded;   // deleteLater'd, deleted explicitly in destructor if still alive
  int NextItemId = 1;
  int NextWorkerId = 1;
  bool Enabled = true;
  bool ProcessScheduled = false;
  bool Destroying = false;
  bool WasActive = false;
  QTimer Housekeeping;

  // options; "Explicit" ones were set by a setter and no longer follow the configuration
  int Limit = 2;
  bool LimitExplicit = false;
  int KeepDoneFor = 15;
  bool KeepDoneExplicit = false;
  int Threshold = -1;
  bool ThresholdExplicit = false;
  int DurationThreshold = DefaultParallelDurationThreshold;
  bool DurationExplicit = false;
  int ReopenDelay = DefaultSessionReopenBackground;
  bool ReopenExplicit = false;

  Item * FindItem(int Id);
  Worker * FindWorker(int Id);
  std::list<Item>::iterator ItemIterator(int Id);
  QList<Worker *> WorkersOf(int ItemId);
  int ConnectionsOf(int ItemId) const;
  int Add(Item && I);
  void ScheduleProcess();
  void Process();
  bool WantsWorker(const Item & I) const;
  bool AnyRunnablePending() const;
  Worker * NewWorker();
  void StartItem(Item & I, Worker & W);
  void StartWork(Item & I, Worker & W);
  void StartTransfer(Item & I, Worker & W);
  void StartDelete(Item & I, Worker & W);
  void StartScan(Item & I, Worker & W);
  void HandleOpened(int WorkerId, const OperationResult & Result);
  void HandleDone(int WorkerId, const OperationResult & Result, const FileOperationProgress::Snapshot & Snapshot);
  void HandleScanned(int WorkerId, const OperationResult & Result, const std::shared_ptr<ScanResult> & Scan);
  void HandleUnitDone(int WorkerId, int UnitIndex, const OperationResult & Result, const FileOperationProgress::Snapshot & Snapshot);
  void DispatchUnit(Item & I, Worker & W);
  int NextUnit(const Item & I) const;
  bool HelpWanted(const Item & I) const;
  void AttachHelper(Item & I, Worker & W);
  void CheckParallel();
  void FinishParallel(std::list<Item>::iterator It);
  void CompleteParallel(int Id);
  void CleanupSegments(const Item & I);
  void FinishItem(std::list<Item>::iterator It, const OperationResult & Result, const FileOperationProgress::Snapshot & Snapshot);
  void FailConnect(std::list<Item>::iterator It, const OperationResult & Result);
  void ScheduleReconnect(Item & I, int DelayMs);
  void ReleaseWorker(Worker & W);
  void DiscardWorker(int WorkerId);
  void MoveToFirstPending(std::list<Item>::iterator It);
  void UpdateState(Item & I);
  unsigned long CPSShare(const Item & I) const;
  void ApplyCPSLimit(Item & I);
  FileOperationProgress::Snapshot ItemProgress(const Item & I) const;
  void UserActionRequested(int WorkerId, const UserActionPtr & Action);
  void CancelActions(Item & I);
  void ExpireDoneItems();
  void RefreshOptions(bool All);
  void CheckEmpty();
  void EmitFinished(int Id, const OperationResult & Result, bool ToRemote, const QString & Target, bool Move);
};

Item * TransferQueue::Private::FindItem(int Id)
{
  for (Item & I : Items)
  {
    if (I.Info.Id == Id)
    {
      return &I;
    }
  }
  return nullptr;
}

std::list<Item>::iterator TransferQueue::Private::ItemIterator(int Id)
{
  return std::find_if(Items.begin(), Items.end(), [Id](const Item & I) { return I.Info.Id == Id; });
}

Worker * TransferQueue::Private::FindWorker(int Id)
{
  for (Worker & W : Workers)
  {
    if (W.Id == Id)
    {
      return &W;
    }
  }
  return nullptr;
}

QList<Worker *> TransferQueue::Private::WorkersOf(int ItemId)
{
  QList<Worker *> Result;
  if (ItemId != 0)
  {
    for (Worker & W : Workers)
    {
      if (W.ItemId == ItemId)
      {
        Result.append(&W);
      }
    }
  }
  return Result;
}

int TransferQueue::Private::ConnectionsOf(int ItemId) const
{
  return static_cast<int>(std::count_if(Workers.cbegin(), Workers.cend(),
    [ItemId](const Worker & W) { return (ItemId != 0) && (W.ItemId == ItemId); }));
}

void TransferQueue::Private::UpdateState(Item & I)
{
  I.Info.Connections = ConnectionsOf(I.Info.Id);
  if (I.Finished)
  {
    return;
  }
  if (!I.Actions.isEmpty())
  {
    I.Info.State = I.Actions.first()->Status;
    I.Info.UserActionText = I.Actions.first()->Text;
    return;
  }
  I.Info.UserActionText.clear();
  bool AnyWorker = false;
  bool AnyOpened = false;
  for (const Worker & W : Workers)
  {
    if (W.ItemId == I.Info.Id)
    {
      AnyWorker = true;
      AnyOpened = AnyOpened || W.Opened;
    }
  }
  if (!AnyWorker)
  {
    if (I.Suspended)
    {
      I.Info.State = QueueItemInfo::qsPaused;
    }
    else
    {
      I.Info.State = (I.Reconnecting || I.Started) ? QueueItemInfo::qsConnecting : QueueItemInfo::qsPending;
    }
  }
  else if (!AnyOpened)
  {
    I.Info.State = QueueItemInfo::qsConnecting;
  }
  else
  {
    I.Info.State = I.Suspended ? QueueItemInfo::qsPaused : QueueItemInfo::qsProcessing;
  }
}

int TransferQueue::Private::Add(Item && I)
{
  I.Info.Id = NextItemId++;
  I.Info.State = QueueItemInfo::qsPending;
  I.Info.CPSLimit = I.Param.CPSLimit;
  int Id = I.Info.Id;
  Items.push_back(std::move(I));
  WasActive = true;
  ScheduleProcess();
  emit Q->Changed();
  return Id;
}

void TransferQueue::Private::ScheduleProcess()
{
  if (!ProcessScheduled && !Destroying)
  {
    ProcessScheduled = true;
    QMetaObject::invokeMethod(Q, [this]()
    {
      ProcessScheduled = false;
      Process();
    }, Qt::QueuedConnection);
  }
}

bool TransferQueue::Private::WantsWorker(const Item & I) const
{
  if (I.Finished || I.Deleted || I.Suspended || !I.Actions.isEmpty())
  {
    return false;
  }
  if (I.Reconnecting && I.RetryTimer.isValid() && !I.RetryTimer.hasExpired(I.RetryDelayMs))
  {
    return false;
  }
  if (!I.Started)
  {
    return Enabled || I.Forced;
  }
  // a started parallel transfer that lost all its connections
  return I.Parallel && I.PS && I.PS->Scanned && !I.PS->Failing && (ConnectionsOf(I.Info.Id) == 0);
}

bool TransferQueue::Private::AnyRunnablePending() const
{
  // TTerminalQueue::ContinueParallelOperation: FItems->Count > FItemsInProcess
  return std::any_of(Items.cbegin(), Items.cend(), [this](const Item & I) { return !I.Started && WantsWorker(I); });
}

Worker * TransferQueue::Private::NewWorker()
{
  Worker NewWorker;
  NewWorker.Id = NextWorkerId++;
  int WorkerId = NewWorker.Id;
  // TBackgroundTerminal: the UI of the connection defers queries to the queue
  auto * UIObject = new WorkerUI(UI, Shared, [this, WorkerId](const UserActionPtr & Action)
  {
    QMetaObject::invokeMethod(Q, [this, WorkerId, Action]() { UserActionRequested(WorkerId, Action); }, Qt::QueuedConnection);
  });
  NewWorker.UI = UIObject;
  NewWorker.Term = new Terminal(Data, UIObject, Q);
  UIObject->setParent(NewWorker.Term);
  // TBackgroundTerminal::DoQueryReopen: a connection lost during an operation
  // is reopened after SessionReopenBackground without asking
  Terminal::Settings Options = Terminal::ConfigurationSettings();
  Options.BackgroundReopen = std::max(1, ReopenDelay);
  NewWorker.Term->SetSettings(Options);
  {
    QMutexLocker Lock(&Shared->Mutex);
    if (!Shared->Password.isEmpty() && (Shared->Password != Shared->DataPassword))
    {
      NewWorker.Term->SetRememberedPassword(Shared->Password);
    }
  }
  QObject::connect(NewWorker.Term, &Terminal::Disconnected, Q, [this, WorkerId](const QString &, const QString &)
  {
    // Lost connection of an idle worker; busy ones are handled when their operation ends
    Worker * Lost = FindWorker(WorkerId);
    if ((Lost != nullptr) && (Lost->ItemId == 0))
    {
      DiscardWorker(WorkerId);
    }
  });
  Workers.push_back(NewWorker);
  return &Workers.back();
}

// TTerminalQueue::ProcessEvent
void TransferQueue::Private::Process()
{
  if (Destroying)
  {
    return;
  }
  bool Any = false;
  // starting an item may change the list, so it is walked by ids
  QList<int> Ids;
  for (const Item & I : Items)
  {
    Ids.append(I.Info.Id);
  }
  for (int Id : Ids)
  {
    auto It = ItemIterator(Id);
    if ((It == Items.end()) || !WantsWorker(*It))
    {
      continue;
    }
    Item & I = *It;

    Worker * W = nullptr;
    for (Worker & Candidate : Workers)
    {
      if ((Candidate.ItemId == 0) && Candidate.Opened)
      {
        W = &Candidate;
        break;
      }
    }
    if ((W == nullptr) && ((static_cast<int>(Workers.size()) < Limit) || I.Forced))
    {
      W = NewWorker();
    }
    if (W == nullptr)
    {
      continue; // no capacity; a later item may still be forced
    }

    I.Forced = false;
    I.Reconnecting = false;
    if (!I.Started)
    {
      I.Started = true;
      // Keep started items before pending ones (FItemsInProcess)
      auto Dest = Items.begin();
      while ((Dest != It) && Dest->Started)
      {
        ++Dest;
      }
      if (Dest != It)
      {
        Items.splice(Dest, Items, It);
      }
    }
    StartItem(I, *W);
    Any = true;
  }

  // Idle connections join parallel transfers (TryAddParallelOperation with FFreeTerminals > 0)
  QList<int> Idle;
  for (const Worker & W : Workers)
  {
    if ((W.ItemId == 0) && W.Opened)
    {
      Idle.append(W.Id);
    }
  }
  for (int WorkerId : Idle)
  {
    Worker * W = FindWorker(WorkerId);
    if ((W == nullptr) || (W->ItemId != 0))
    {
      continue;
    }
    for (Item & I : Items)
    {
      if (HelpWanted(I))
      {
        AttachHelper(I, *W);
        Any = true;
        break;
      }
    }
  }
  if (Any)
  {
    emit Q->Changed();
  }
}

void TransferQueue::Private::StartItem(Item & I, Worker & W)
{
  W.ItemId = I.Info.Id;
  W.Helper = false;
  W.UnitIndex = -1;
  W.UI->SetItem(I.Shared);
  if (W.Opened)
  {
    W.Reused = true;
    StartWork(I, W);
  }
  else
  {
    W.Reused = false;
    UpdateState(I);
    int WorkerId = W.Id;
    W.Term->Open([this, WorkerId](const OperationResult & Result)
    {
      QMetaObject::invokeMethod(Q, [this, WorkerId, Result]() { HandleOpened(WorkerId, Result); }, Qt::QueuedConnection);
    });
  }
}

void TransferQueue::Private::StartWork(Item & I, Worker & W)
{
  // Suspended flag is not reset by FileOperationProgress::Start, and the
  // connection may be reused from a suspended item
  if (I.Suspended)
  {
    W.Term->Progress().Suspend();
  }
  else
  {
    W.Term->Progress().Resume();
  }
  if (I.Info.Type == QueueItemInfo::ikDelete)
  {
    StartDelete(I, W);
  }
  else if (I.Parallel)
  {
    if (I.PS && I.PS->Scanned)
    {
      ApplyCPSLimit(I);
      DispatchUnit(I, W);
    }
    else
    {
      StartScan(I, W);
    }
  }
  else
  {
    StartTransfer(I, W);
  }
  UpdateState(I);
}

void TransferQueue::Private::StartTransfer(Item & I, Worker & W)
{
  int WorkerId = W.Id;
  Terminal * Term = W.Term;
  auto Done = [this, WorkerId, Term](const OperationResult & Result)
  {
    FileOperationProgress::Snapshot Snapshot = Term->Progress().Get();
    QMetaObject::invokeMethod(Q, [this, WorkerId, Result, Snapshot]() { HandleDone(WorkerId, Result, Snapshot); }, Qt::QueuedConnection);
  };
  CopyParam Param = I.Param;
  Param.CPSLimit = I.Info.CPSLimit;
  if (I.Info.ToRemote)
  {
    W.Term->CopyToRemote(I.Info.Sources, I.Info.TargetDirectory, Param, I.Info.Move, Done);
  }
  else
  {
    W.Term->CopyToLocal(I.Info.Sources, I.Info.TargetDirectory, Param, I.Info.Move, Done);
  }
}

void TransferQueue::Private::StartDelete(Item & I, Worker & W)
{
  int WorkerId = W.Id;
  Terminal * Term = W.Term;
  auto Done = [this, WorkerId, Term](const OperationResult & Result)
  {
    FileOperationProgress::Snapshot Snapshot = Term->Progress().Get();
    QMetaObject::invokeMethod(Q, [this, WorkerId, Result, Snapshot]() { HandleDone(WorkerId, Result, Snapshot); }, Qt::QueuedConnection);
  };
  if (I.Info.ToRemote)
  {
    // TRemoteDeleteQueueItem
    W.Term->DeleteFiles(I.Info.Sources, Done);
  }
  else
  {
    // TLocalDeleteQueueItem (TTerminal::DeleteLocalFiles; Recycle Bin -> desktop trash)
    QStringList Paths = I.Info.Sources;
    bool ToTrash = I.ToTrash;
    W.Term->Progress().Start(FileOperationProgress::Delete, FileOperationProgress::ToLocal, static_cast<int>(Paths.size()));
    W.Term->RunOnWorker([Paths, ToTrash](FileSystem &, FileOperationProgress & Progress, UserInterface &)
    {
      for (const QString & Path : Paths)
      {
        Progress.CheckCancel();
        Progress.SetFile(Path, 0);
        QFileInfo Info(Path);
        bool Ok;
        if (ToTrash)
        {
          Ok = LocalFileOperations::MoveToTrash(Path);
        }
        else if (Info.isDir() && !Info.isSymLink())
        {
          Ok = QDir(Path).removeRecursively();
        }
        else
        {
          Ok = QFile::remove(Path);
        }
        if (!Ok)
        {
          throw ExtException(QStringLiteral("Error deleting file '%1'.").arg(Path));
        }
        Progress.FileFinished();
      }
    }, Done);
  }
}

// TTransferQueueItem::DoExecute -> CopyToRemote/CopyToLocal with a
// TParallelOperation: collect the files first
void TransferQueue::Private::StartScan(Item & I, Worker & W)
{
  if (!I.PS)
  {
    I.PS = std::make_shared<ParallelState>();
    I.PS->Elapsed.start();
    I.PS->LastClientAdded.start();
  }
  I.Info.Listing = true;
  int WorkerId = W.Id;
  bool ToRemote = I.Info.ToRemote;
  QStringList Sources = I.Info.Sources;
  QString TargetDir = I.Info.TargetDirectory;
  CopyParam Param = I.Param;
  // TTerminal::CanParallel (fcParallelTransfers) / CheckParallelFileTransfer (fcParallelFileTransfers)
  bool CanParallel = W.Term->IsCapable(fcParallelTransfers);
  int ThresholdKB = ((Limit > 1) && W.Term->IsCapable(fcParallelFileTransfers)) ? Threshold : 0;
  auto Scan = std::make_shared<ScanResult>();
  W.Term->Progress().Start(FileOperationProgress::Calculate,
    ToRemote ? FileOperationProgress::ToRemote : FileOperationProgress::ToLocal, static_cast<int>(Sources.size()));
  W.Term->Progress().SetCPSLimit(I.Info.CPSLimit);
  W.Term->RunOnWorker([Scan, ToRemote, Sources, TargetDir, Param, ThresholdKB, CanParallel](FileSystem & Fs, FileOperationProgress & Progress, UserInterface & AUI)
  {
    // not SCP (does not implement cpNoRecurse)
    if (!CanParallel || (dynamic_cast<ScpFileSystem *>(&Fs) != nullptr))
    {
      Scan->Parallel = false;
      return;
    }
    if (ToRemote)
    {
      QSet<QString> Visited;
      for (const QString & Path : Sources)
      {
        ScanLocal(Progress, Path, UnixPath::Combine(TargetDir, CopyParam::RestoreChars(Param.ChangeFileName(QFileInfo(Path).fileName(), true))), -1,
          Param, Visited, Scan->Entries);
      }
      return;
    }
    RemoteFile OnlyFile;
    for (const QString & Path : Sources)
    {
      RemoteFile File = Fs.ReadFile(Path);
      if (Sources.size() == 1)
      {
        OnlyFile = File;
      }
      ScanRemote(Fs, Progress, Path, File, QDir(TargetDir).filePath(CopyParam::ValidLocalFileName(Param.ChangeFileName(UnixPath::ExtractFileName(Path), true))),
        -1, Param, Scan->Entries);
    }
    // Local directories are created right away; files wait for nothing
    for (const ScanEntry & Entry : Scan->Entries)
    {
      if (Entry.Directory && !QDir().mkpath(Entry.Target))
      {
        throw ExtException(QStringLiteral("Cannot create local folder '%1'.").arg(Entry.Target));
      }
    }
    // TTerminal::CheckParallelFileTransfer
    if ((ThresholdKB > 0) && (Scan->Entries.size() == 1) && !Scan->Entries[0].Directory && !OnlyFile.IsSymLink &&
        (dynamic_cast<SftpFileSystem *>(&Fs) != nullptr) &&
        (Scan->Entries[0].Size >= static_cast<qint64>(ThresholdKB) * 1024) &&
        !Param.UseAsciiTransfer(Scan->Entries[0].Source, false, MaskParams(OnlyFile.Size, OnlyFile.Modification)))
    {
      const ScanEntry & Entry = Scan->Entries[0];
      Scan->Modification = OnlyFile.Modification;
      QFileInfo Existing(Entry.Target);
      if (Existing.exists())
      {
        if (Param.NewerOnly)
        {
          if (OnlyFile.Modification.toSecsSinceEpoch() <= Existing.lastModified().toSecsSinceEpoch())
          {
            Scan->Skip = true;
            return;
          }
        }
        else if (Param.ConfirmOverwriting)
        {
          QLocale Locale;
          // FILE_OVERWRITE + FILE_OVERWRITE_DETAILS
          QString Query = QStringLiteral("File '%1' already exists. Overwrite?").arg(QFileInfo(Entry.Target).fileName());
          QString Details = QStringLiteral("New:      \t%1 bytes, %2\nExisting: \t%3 bytes, %4")
            .arg(Locale.toString(OnlyFile.Size), Locale.toString(OnlyFile.Modification, QLocale::ShortFormat),
                 Locale.toString(Existing.size()), Locale.toString(Existing.lastModified(), QLocale::ShortFormat));
          unsigned int Answer = AUI.QueryUser(Query, Details, qaYes | qaNo | qaCancel);
          if (Answer == qaNo)
          {
            Scan->Skip = true;
            return;
          }
          if (Answer != qaYes)
          {
            throw AbortException();
          }
        }
      }
      Scan->Segment = true;
    }
  },
  [this, WorkerId, Scan](const OperationResult & Result)
  {
    QMetaObject::invokeMethod(Q, [this, WorkerId, Result, Scan]() { HandleScanned(WorkerId, Result, Scan); }, Qt::QueuedConnection);
  });
}

void TransferQueue::Private::HandleScanned(int WorkerId, const OperationResult & Result, const std::shared_ptr<ScanResult> & Scan)
{
  if (Destroying)
  {
    return;
  }
  Worker * W = FindWorker(WorkerId);
  if (W == nullptr)
  {
    return;
  }
  auto It = ItemIterator(W->ItemId);
  if (It != Items.end())
  {
    It->Info.Listing = false;
  }
  if (!Result.Ok || (It == Items.end()) || It->Deleted)
  {
    // handled as the end of a plain transfer
    HandleDone(WorkerId, Result, W->Term->Progress().Get());
    return;
  }
  Item & I = *It;
  if (!Scan->Parallel)
  {
    I.Parallel = false;
    I.PS.reset();
    StartTransfer(I, *W);
    UpdateState(I);
    emit Q->Changed();
    return;
  }
  ParallelState & PS = *I.PS;
  PS.Scanned = true;
  PS.Units.clear();
  if (Scan->Skip)
  {
    // nothing to do
  }
  else if (Scan->Segment)
  {
    const ScanEntry & Entry = Scan->Entries[0];
    PS.Segmented = true;
    PS.TotalSize = Entry.Size;
    PS.FileCount = 1;
    auto Seg = std::make_shared<SegmentState>();
    Seg->RemotePath = Entry.Source;
    Seg->TargetPath = Entry.Target;
    Seg->Size = Entry.Size;
    Seg->Modification = Scan->Modification;
    Seg->PreserveTime = I.Param.PreserveTime;
    // TParallelOperation::GetNext: PartSize = size / QueueTransfersLimit,
    // the last part goes until the end (also when less than 10 % would remain)
    qint64 PartSize = std::max<qint64>(1, Entry.Size / std::max(1, Limit));
    qint64 Offset = 0;
    int Index = 0;
    for (;;)
    {
      Unit U;
      U.Kind = Unit::ukPart;
      U.Source = Entry.Source;
      U.Target = PartFileName(Entry.Target, Index);
      U.Index = Index;
      U.Offset = Offset;
      qint64 Remaining = Entry.Size - Offset;
      if ((PartSize >= Remaining) || (Remaining - PartSize < PartSize / 10))
      {
        U.Length = -1;
        U.Size = Remaining;
        PS.Units.push_back(U);
        break;
      }
      U.Length = PartSize;
      U.Size = PartSize;
      PS.Units.push_back(U);
      Offset += PartSize;
      Index++;
    }
    Seg->PartDone.assign(PS.Units.size(), false);
    PS.Segment = Seg;
  }
  else
  {
    // entry index -> unit index of directories to create (uploads)
    std::vector<int> DirUnit(Scan->Entries.size(), -1);
    for (size_t Index = 0; Index < Scan->Entries.size(); Index++)
    {
      const ScanEntry & Entry = Scan->Entries[Index];
      int Parent = (Entry.Parent >= 0) ? DirUnit[static_cast<size_t>(Entry.Parent)] : -1;
      Unit U;
      U.Source = Entry.Source;
      U.Parent = Parent;
      if (Entry.Directory)
      {
        if (!I.Info.ToRemote)
        {
          continue; // created by the listing already
        }
        U.Kind = Unit::ukDir;
        U.Target = Entry.Target;
        U.Mode = I.Param.PreserveRights ? I.Param.RemoteFileRights(true) : 0755;
        DirUnit[Index] = static_cast<int>(PS.Units.size());
      }
      else
      {
        U.Kind = Unit::ukFile;
        U.Size = Entry.Size;
        U.Target = I.Info.ToRemote ? UnixPath::ExtractFileDir(Entry.Target) : QFileInfo(Entry.Target).path();
        if (I.Info.ToRemote && U.Target.isEmpty())
        {
          U.Target = QStringLiteral("/");
        }
        PS.TotalSize += Entry.Size;
        PS.FileCount++;
      }
      PS.Units.push_back(U);
    }
  }
  PS.FirstPending = 0;
  ApplyCPSLimit(I);
  int Id = I.Info.Id;
  DispatchUnit(I, *W);
  // idle connections may join now
  ScheduleProcess();
  Item * Current = FindItem(Id);
  if (Current != nullptr)
  {
    UpdateState(*Current);
  }
  emit Q->Changed();
}

// TParallelOperation::GetNext: first pending unit whose directory exists
int TransferQueue::Private::NextUnit(const Item & I) const
{
  if (!I.PS)
  {
    return -1;
  }
  const ParallelState & PS = *I.PS;
  for (size_t Index = PS.FirstPending; Index < PS.Units.size(); Index++)
  {
    const Unit & U = PS.Units[Index];
    if ((U.Status == Unit::usPending) &&
        ((U.Parent < 0) || (PS.Units[static_cast<size_t>(U.Parent)].Status == Unit::usDone)))
    {
      return static_cast<int>(Index);
    }
  }
  return -1;
}

void TransferQueue::Private::DispatchUnit(Item & I, Worker & W)
{
  ParallelState & PS = *I.PS;
  int Index = -1;
  if (!PS.Failing && !I.Deleted &&
      // a joined connection leaves for a waiting item (ContinueParallelOperation)
      !(W.Helper && Enabled && AnyRunnablePending()))
  {
    Index = NextUnit(I);
  }
  if (Index < 0)
  {
    W.UnitIndex = -1;
    bool Complete = (PS.Running == 0) && (PS.DoneCount == static_cast<int>(PS.Units.size())) && !PS.Failing && !I.Deleted;
    if (Complete)
    {
      // everything transferred; finished from the event loop, so that no
      // caller continues with an item (or queue) removed by ItemFinished;
      // the connection stays with the item until then
      int Id = I.Info.Id;
      QMetaObject::invokeMethod(Q, [this, Id]() { CompleteParallel(Id); }, Qt::QueuedConnection);
      return;
    }
    // nothing for this connection now (waiting for a directory, or the rest is running)
    ReleaseWorker(W);
    ScheduleProcess();
    return;
  }
  Unit & U = PS.Units[static_cast<size_t>(Index)];
  U.Status = Unit::usRunning;
  PS.Running++;
  while ((PS.FirstPending < PS.Units.size()) && (PS.Units[PS.FirstPending].Status != Unit::usPending))
  {
    PS.FirstPending++;
  }
  W.UnitIndex = Index;
  if (I.Suspended)
  {
    W.Term->Progress().Suspend();
  }
  else
  {
    W.Term->Progress().Resume();
  }
  int WorkerId = W.Id;
  Terminal * Term = W.Term;
  auto Done = [this, WorkerId, Index, Term](const OperationResult & Result)
  {
    FileOperationProgress::Snapshot Snapshot = Term->Progress().Get();
    QMetaObject::invokeMethod(Q, [this, WorkerId, Index, Result, Snapshot]() { HandleUnitDone(WorkerId, Index, Result, Snapshot); },
      Qt::QueuedConnection);
  };
  unsigned long Share = CPSShare(I);
  FileOperationProgress::Side Side = I.Info.ToRemote ? FileOperationProgress::ToRemote : FileOperationProgress::ToLocal;
  switch (U.Kind)
  {
    case Unit::ukFile:
      {
        // TTerminal::CopyToParallel
        CopyParam Param = I.Param;
        Param.CPSLimit = Share;
        if (I.Info.ToRemote)
        {
          W.Term->CopyToRemote({U.Source}, U.Target, Param, false, Done);
        }
        else
        {
          W.Term->CopyToLocal({U.Source}, U.Target, Param, false, Done);
        }
      }
      break;
    case Unit::ukDir:
      {
        QString Target = U.Target;
        int Mode = static_cast<int>(U.Mode);
        W.Term->Progress().Start(FileOperationProgress::Copy, Side, 1);
        W.Term->Progress().SetCPSLimit(Share);
        W.Term->Progress().SetFile(U.Source, 0);
        W.Term->RunOnWorker([Target, Mode](FileSystem & Fs, FileOperationProgress & Progress, UserInterface &)
        {
          Progress.CheckCancel();
          RemoteFile Existing;
          if (!Fs.FileExists(Target, &Existing))
          {
            Fs.CreateDirectory(Target, Mode);
          }
        }, Done);
      }
      break;
    case Unit::ukPart:
      {
        SegmentStatePtr Seg = PS.Segment;
        int PartIndex = U.Index;
        qint64 Offset = U.Offset;
        qint64 Length = U.Length;
        W.Term->Progress().Start(FileOperationProgress::Copy, Side, 1);
        W.Term->Progress().SetCPSLimit(Share);
        W.Term->Progress().SetFile(U.Source, U.Size);
        W.Term->RunOnWorker([Seg, PartIndex, Offset, Length](FileSystem & Fs, FileOperationProgress & Progress, UserInterface &)
        {
          DownloadPart(Fs, Progress, Seg, PartIndex, Offset, Length);
        }, Done);
      }
      break;
  }
}

void TransferQueue::Private::HandleUnitDone(int WorkerId, int UnitIndex, const OperationResult & Result,
  const FileOperationProgress::Snapshot & Snapshot)
{
  if (Destroying)
  {
    return;
  }
  Worker * W = FindWorker(WorkerId);
  if (W == nullptr)
  {
    return;
  }
  auto It = ItemIterator(W->ItemId);
  if ((It == Items.end()) || !It->PS || (UnitIndex < 0) || (UnitIndex >= static_cast<int>(It->PS->Units.size())))
  {
    ReleaseWorker(*W);
    ScheduleProcess();
    return;
  }
  Item & I = *It;
  ParallelState & PS = *I.PS;
  Unit & U = PS.Units[static_cast<size_t>(UnitIndex)];
  W->UnitIndex = -1;
  PS.Running--;
  bool LostConnection = false;
  if (Result.Ok)
  {
    U.Status = Unit::usDone;
    PS.DoneCount++;
    PS.FatalFailures = 0;
    if (U.Kind == Unit::ukFile)
    {
      PS.DoneTransferred += Snapshot.TotalTransferred;
      PS.DoneFiles += Snapshot.FilesFinished;
    }
    else if (U.Kind == Unit::ukPart)
    {
      PS.DoneTransferred += Snapshot.TotalTransferred;
      if (PS.DoneCount == static_cast<int>(PS.Units.size()))
      {
        PS.DoneFiles = 1;
      }
    }
    else
    {
      // the directory exists: its contents may be taken by idle connections
      ScheduleProcess();
    }
  }
  else if (Result.Fatal && !I.Deleted && !PS.Failing)
  {
    // the unit is done again on another connection, resuming where possible
    U.Status = Unit::usPending;
    PS.FirstPending = std::min(PS.FirstPending, static_cast<size_t>(UnitIndex));
    PS.FatalFailures++;
    LostConnection = true;
    if (PS.FatalFailures > MaxParallelFatalFailures)
    {
      PS.Failing = true;
      PS.Failure = Result;
    }
  }
  else
  {
    U.Status = Unit::usPending;
    if (!PS.Failing)
    {
      // any failed part cancels the whole transfer
      PS.Failing = true;
      PS.Failure = Result;
      for (Worker * Other : WorkersOf(I.Info.Id))
      {
        if ((Other != W) && (Other->UnitIndex >= 0))
        {
          Other->Term->Progress().Cancel();
        }
      }
      CancelActions(I);
    }
  }

  if (LostConnection || Result.Fatal)
  {
    DiscardWorker(WorkerId);
    W = nullptr;
  }
  if (PS.Failing || I.Deleted)
  {
    if (W != nullptr)
    {
      ReleaseWorker(*W);
    }
    bool AnyRunning = false;
    for (Worker * Other : WorkersOf(I.Info.Id))
    {
      if (Other->UnitIndex >= 0)
      {
        AnyRunning = true;
      }
      else
      {
        ReleaseWorker(*Other);
      }
    }
    if (!AnyRunning && (PS.Running == 0))
    {
      FinishParallel(It);
    }
    else
    {
      UpdateState(I);
      emit Q->Changed();
    }
    ScheduleProcess();
    return;
  }
  int Id = I.Info.Id;
  if (W != nullptr)
  {
    DispatchUnit(I, *W);
  }
  else if (ConnectionsOf(Id) == 0)
  {
    // all connections lost: reopen after SessionReopenBackground (TBackgroundTerminal::DoQueryReopen)
    ScheduleReconnect(I, ReopenDelay);
  }
  Item * Current = FindItem(Id);
  if (Current != nullptr)
  {
    ApplyCPSLimit(*Current);
    UpdateState(*Current);
  }
  ScheduleProcess();
  if (LostConnection)
  {
    emit Q->Changed();
  }
}

// TTransferQueueItem::ProgressUpdated + TParallelOperation::ShouldAddClient
bool TransferQueue::Private::HelpWanted(const Item & I) const
{
  if (!I.Started || I.Finished || I.Deleted || I.Suspended || !I.Parallel || !I.PS || !I.PS->Scanned || I.PS->Failing)
  {
    return false;
  }
  if (ConnectionsOf(I.Info.Id) == 0)
  {
    return false; // reconnecting: WantsWorker takes care
  }
  // more work than the connections already on it
  return NextUnit(I) >= 0;
}

void TransferQueue::Private::AttachHelper(Item & I, Worker & W)
{
  // TTerminalQueue::TryAddParallelOperation / TParallelTransferQueueItem
  W.ItemId = I.Info.Id;
  W.Helper = true;
  W.UnitIndex = -1;
  W.UI->SetItem(I.Shared);
  I.PS->LastClientAdded.start();
  int Id = I.Info.Id;
  if (W.Opened)
  {
    W.Reused = true;
    ApplyCPSLimit(I);
    DispatchUnit(I, W);
  }
  else
  {
    W.Reused = false;
    int WorkerId = W.Id;
    W.Term->Open([this, WorkerId](const OperationResult & Result)
    {
      QMetaObject::invokeMethod(Q, [this, WorkerId, Result]() { HandleOpened(WorkerId, Result); }, Qt::QueuedConnection);
    });
  }
  Item * Current = FindItem(Id);
  if (Current != nullptr)
  {
    UpdateState(*Current);
  }
}

// Opens a new connection for a parallel transfer that is expected to take
// long enough (TTransferQueueItem::ProgressUpdated "Force")
void TransferQueue::Private::CheckParallel()
{
  if (static_cast<int>(Workers.size()) >= Limit)
  {
    return;
  }
  if (std::any_of(Workers.cbegin(), Workers.cend(), [](const Worker & W) { return (W.ItemId == 0) && W.Opened; }))
  {
    ScheduleProcess(); // idle connections join in Process
    return;
  }
  if (AnyRunnablePending())
  {
    return;
  }
  for (Item & I : Items)
  {
    if (!HelpWanted(I) || !I.Actions.isEmpty() || !I.PS->LastClientAdded.hasExpired(ParallelAddIntervalMs))
    {
      continue;
    }
    FileOperationProgress::Snapshot S = ItemProgress(I);
    qint64 TimeLeft = 0;
    if ((S.ElapsedMs > 0) && (S.TotalTransferred > 0) && (S.TotalSize > S.TotalTransferred))
    {
      qint64 CPS = S.TotalTransferred * 1000 / S.ElapsedMs;
      TimeLeft = (CPS > 0) ? ((S.TotalSize - S.TotalTransferred) / CPS) : 0;
    }
    if (TimeLeft >= DurationThreshold)
    {
      Worker * W = NewWorker();
      AttachHelper(I, *W);
      emit Q->Changed();
      return;
    }
  }
}

void TransferQueue::Private::CleanupSegments(const Item & I)
{
  if (!I.PS || !I.PS->Segment)
  {
    return;
  }
  const SegmentState & Seg = *I.PS->Segment;
  for (size_t Index = 0; Index < I.PS->Units.size(); Index++)
  {
    QFile::remove(PartFileName(Seg.TargetPath, static_cast<int>(Index)));
  }
  QFile::remove(Seg.TargetPath + PartialExt);
}

void TransferQueue::Private::CompleteParallel(int Id)
{
  if (Destroying)
  {
    return;
  }
  auto It = ItemIterator(Id);
  if ((It == Items.end()) || It->Finished || !It->PS)
  {
    return;
  }
  const ParallelState & PS = *It->PS;
  if ((PS.Running != 0) || (PS.DoneCount != static_cast<int>(PS.Units.size())))
  {
    return;
  }
  if (PS.Failing || It->Deleted)
  {
    FinishParallel(It);
    return;
  }
  OperationResult Ok;
  FinishItem(It, Ok, ItemProgress(*It));
}

// Failed, cancelled or deleted parallel transfer whose connections all returned
void TransferQueue::Private::FinishParallel(std::list<Item>::iterator It)
{
  Item & I = *It;
  for (Worker * W : WorkersOf(I.Info.Id))
  {
    ReleaseWorker(*W);
  }
  CleanupSegments(I);
  OperationResult Result = I.PS ? I.PS->Failure : OperationResult();
  if (I.Deleted)
  {
    Result.Ok = false;
    Result.Aborted = true;
  }
  FinishItem(It, Result, ItemProgress(I));
}

void TransferQueue::Private::FinishItem(std::list<Item>::iterator It, const OperationResult & Result,
  const FileOperationProgress::Snapshot & Snapshot)
{
  Item & I = *It;
  for (Worker * W : WorkersOf(I.Info.Id))
  {
    ReleaseWorker(*W);
  }
  CancelActions(I);
  int Id = I.Info.Id;
  bool ToRemote = I.Info.ToRemote;
  QString Target = I.Info.TargetDirectory;
  bool Move = I.Info.Move;
  I.Reconnecting = false;
  I.Info.Listing = false;
  bool Remove = false;
  if (I.Deleted || (Result.Aborted && !Result.Fatal))
  {
    // Cancelled by Delete, or by the user at a prompt: the item is gone (as in WinSCP)
    Remove = true;
  }
  else
  {
    I.Finished = true;
    I.Info.Finished = true;
    I.Info.Progress = Snapshot;
    I.Info.Progress.InProgress = false;
    I.Info.Connections = 0;
    if (Result.Ok)
    {
      I.Info.State = QueueItemInfo::qsDone;
      I.DoneTimer.start();
      // TTerminalQueue::DeleteItem(Item, CanKeep): not kept when KeepDoneItemsFor is 0
      Remove = (KeepDoneFor == 0);
    }
    else
    {
      I.Info.State = QueueItemInfo::qsError;
      I.Info.Error = Result.Message.isEmpty() ? QStringLiteral("Operation was aborted") : Result.Message;
      I.Info.MoreMessages = Result.MoreMessages;
    }
  }
  if (Remove)
  {
    Items.erase(It);
  }
  ScheduleProcess();
  EmitFinished(Id, Result, ToRemote, Target, Move);
}

void TransferQueue::Private::EmitFinished(int Id, const OperationResult & Result, bool ToRemote, const QString & Target, bool Move)
{
  QPointer<TransferQueue> Guard(Q);
  emit Q->Changed();
  if (Guard)
  {
    emit Q->ItemFinished(Id, Result, ToRemote, Target, Move);
  }
  if (Guard)
  {
    CheckEmpty();
  }
}

void TransferQueue::Private::FailConnect(std::list<Item>::iterator It, const OperationResult & Result)
{
  // TTerminalItem::ProcessEvent: the error is shown (Show Error), then the item is retried
  It->ConnectFailed = true;
  OperationResult R = Result;
  if (R.Message.isEmpty())
  {
    R.Message = QStringLiteral("Connection failed.");
  }
  FinishItem(It, R, It->Info.Progress);
}

void TransferQueue::Private::ScheduleReconnect(Item & I, int DelayMs)
{
  I.Reconnecting = true;
  I.Forced = true;
  I.RetryDelayMs = std::max(0, DelayMs);
  I.RetryTimer.start();
  UpdateState(I);
  QTimer::singleShot(I.RetryDelayMs + 1, Q, [this]() { ScheduleProcess(); });
}

void TransferQueue::Private::HandleOpened(int WorkerId, const OperationResult & Result)
{
  if (Destroying)
  {
    return;
  }
  Worker * W = FindWorker(WorkerId);
  if (W == nullptr)
  {
    return;
  }
  auto It = ItemIterator(W->ItemId);
  if (Result.Ok)
  {
    W->Opened = true;
    if ((It == Items.end()) || It->Deleted || It->Finished || (It->PS && It->PS->Failing))
    {
      bool Helper = W->Helper;
      ReleaseWorker(*W);
      if ((It != Items.end()) && !Helper && (ConnectionsOf(It->Info.Id) == 0))
      {
        if (It->Deleted)
        {
          Items.erase(It);
        }
        else if (It->PS && It->PS->Failing)
        {
          FinishParallel(It);
          return;
        }
      }
      ScheduleProcess();
      emit Q->Changed();
      return;
    }
    StartWork(*It, *W);
    emit Q->Changed();
    return;
  }

  // Connection failed
  bool Helper = W->Helper;
  DiscardWorker(WorkerId);
  ScheduleProcess();
  if (It == Items.end())
  {
    emit Q->Changed();
    return;
  }
  if (Helper && (ConnectionsOf(It->Info.Id) > 0))
  {
    // a joining connection failed; the transfer goes on with the others
    UpdateState(*It);
    emit Q->Changed();
    return;
  }
  if (It->Deleted)
  {
    Items.erase(It);
    emit Q->Changed();
    CheckEmpty();
    return;
  }
  if (It->PS && It->PS->Failing)
  {
    FinishParallel(It);
    return;
  }
  if (Result.Aborted)
  {
    // cancelled by the user at a prompt of the connection
    FinishItem(It, Result, It->Info.Progress);
    return;
  }
  FailConnect(It, Result);
}

void TransferQueue::Private::HandleDone(int WorkerId, const OperationResult & Result, const FileOperationProgress::Snapshot & Snapshot)
{
  if (Destroying)
  {
    return;
  }
  Worker * W = FindWorker(WorkerId);
  if (W == nullptr)
  {
    return;
  }
  bool Reused = W->Reused;
  auto It = ItemIterator(W->ItemId);
  if (Result.Fatal)
  {
    DiscardWorker(WorkerId);
  }
  else
  {
    ReleaseWorker(*W);
  }
  W = nullptr;
  ScheduleProcess();

  if (It == Items.end())
  {
    emit Q->Changed();
    return;
  }

  Item & I = *It;
  if (Result.Fatal && !I.Deleted && !I.Finished)
  {
    // TBackgroundTerminal::DoQueryReopen: reopen the lost connection after
    // SessionReopenBackground and continue (transfers resume .filepart files),
    // as long as the previous attempt made progress
    bool Progressed = (Snapshot.TotalTransferred > I.PrevTransferred) || (Snapshot.FilesFinished > I.PrevFiles);
    bool NothingDone = (Snapshot.TotalTransferred == 0) && (Snapshot.FilesFinished == 0);
    if (Progressed || (NothingDone && (I.PrevTransferred < 0)))
    {
      I.PrevTransferred = Snapshot.TotalTransferred;
      I.PrevFiles = Snapshot.FilesFinished;
      I.Info.Progress = Snapshot;
      I.Started = false;
      MoveToFirstPending(It);
      // a reused connection found dead before anything was done (closed by
      // the server while idle) is replaced at once
      ScheduleReconnect(I, (Reused && NothingDone) ? 0 : ReopenDelay);
      emit Q->Changed();
      return;
    }
  }
  if (I.Parallel && I.PS && !I.PS->Scanned && (I.Info.Type == QueueItemInfo::ikTransfer))
  {
    // listing failed
    I.PS.reset();
  }
  FinishItem(It, Result, Snapshot);
}

void TransferQueue::Private::ReleaseWorker(Worker & W)
{
  int ItemId = W.ItemId;
  W.ItemId = 0;
  W.Reused = false;
  W.Helper = false;
  W.UnitIndex = -1;
  W.UI->SetItem(ItemSharedPtr());
  if (static_cast<int>(Workers.size()) > Limit)
  {
    // Temporary connection of "Execute Now", or limit was lowered
    DiscardWorker(W.Id);
  }
  Item * I = FindItem(ItemId);
  if (I != nullptr)
  {
    I->Info.Connections = ConnectionsOf(ItemId);
  }
}

void TransferQueue::Private::DiscardWorker(int WorkerId)
{
  auto It = std::find_if(Workers.begin(), Workers.end(), [WorkerId](const Worker & W) { return W.Id == WorkerId; });
  if (It == Workers.end())
  {
    return;
  }
  Terminal * Term = It->Term;
  WorkerUI * WUI = It->UI;
  Workers.erase(It);
  QObject::disconnect(Term, nullptr, Q, nullptr);
  // a thread blocked in a user action must not keep the Terminal alive
  WUI->Close();
  // Possibly called from within the Terminal's own signal: delete later
  Term->Close();
  Term->deleteLater();
  Discarded.removeAll(QPointer<Terminal>());
  Discarded.append(QPointer<Terminal>(Term));
}

void TransferQueue::Private::MoveToFirstPending(std::list<Item>::iterator It)
{
  auto Dest = Items.begin();
  while ((Dest != Items.end()) && ((Dest == It) || Dest->Started))
  {
    ++Dest;
  }
  if (Dest != It)
  {
    Items.splice(Dest, Items, It);
  }
}

// Parallel connections share the speed limit of the item
unsigned long TransferQueue::Private::CPSShare(const Item & I) const
{
  if (I.Info.CPSLimit == 0)
  {
    return 0;
  }
  int Count = std::max(1, ConnectionsOf(I.Info.Id));
  return std::max<unsigned long>(1, I.Info.CPSLimit / static_cast<unsigned long>(Count));
}

void TransferQueue::Private::ApplyCPSLimit(Item & I)
{
  unsigned long Share = CPSShare(I);
  for (Worker * W : WorkersOf(I.Info.Id))
  {
    W->Term->Progress().SetCPSLimit(Share);
  }
}

FileOperationProgress::Snapshot TransferQueue::Private::ItemProgress(const Item & I) const
{
  if (I.Finished)
  {
    return I.Info.Progress;
  }
  if (I.Parallel && I.PS && I.PS->Scanned)
  {
    // sum of the connections (TParallelOperation's MainOperationProgress)
    const ParallelState & PS = *I.PS;
    FileOperationProgress::Snapshot S{};
    S.Op = FileOperationProgress::Copy;
    S.Dir = I.Info.ToRemote ? FileOperationProgress::ToRemote : FileOperationProgress::ToLocal;
    S.TotalSize = PS.TotalSize;
    S.Count = PS.FileCount;
    S.TotalTransferred = PS.DoneTransferred;
    S.FilesFinished = PS.DoneFiles;
    S.ElapsedMs = PS.Elapsed.isValid() ? PS.Elapsed.elapsed() : 0;
    S.InProgress = true;
    bool HaveFile = false;
    for (const Worker & W : Workers)
    {
      if ((W.ItemId != I.Info.Id) || (W.UnitIndex < 0))
      {
        continue;
      }
      FileOperationProgress::Snapshot WS = W.Term->Progress().Get();
      S.TotalTransferred += WS.TotalTransferred;
      if (!PS.Segmented)
      {
        S.FilesFinished += WS.FilesFinished;
      }
      if (!HaveFile && !WS.FileName.isEmpty())
      {
        HaveFile = true;
        S.FileName = WS.FileName;
        S.FileSize = WS.FileSize;
        S.FileTransferred = WS.FileTransferred;
      }
    }
    if (PS.Segmented)
    {
      S.FileName = PS.Segment ? PS.Segment->RemotePath : QString();
      S.FileSize = PS.TotalSize;
      S.FileTransferred = S.TotalTransferred;
    }
    return S;
  }
  for (const Worker & W : Workers)
  {
    if ((W.ItemId == I.Info.Id) && W.Opened)
    {
      return W.Term->Progress().Get();
    }
  }
  return I.Info.Progress;
}

// TTerminalItem::WaitForUserAction (GUI side)
void TransferQueue::Private::UserActionRequested(int WorkerId, const UserActionPtr & Action)
{
  Worker * W = FindWorker(WorkerId);
  Item * I = ((W != nullptr) && !Destroying) ? FindItem(W->ItemId) : nullptr;
  if ((I == nullptr) || I->Deleted || I->Finished || (I->PS && I->PS->Failing))
  {
    // so far query without queue item can occur only for key confirmation
    // on re-key with non-cached host key. make it fail.
    Action->Complete(true);
    return;
  }
  I->Actions.append(Action);
  int Id = I->Info.Id;
  UpdateState(*I);
  QPointer<TransferQueue> Guard(Q);
  emit Q->Changed();
  if (Guard)
  {
    emit Q->UserActionPending(Id);
  }
}

void TransferQueue::Private::CancelActions(Item & I)
{
  QList<UserActionPtr> Actions = I.Actions;
  I.Actions.clear();
  for (const UserActionPtr & Action : Actions)
  {
    Action->Complete(true);
  }
}

void TransferQueue::Private::ExpireDoneItems()
{
  if (KeepDoneFor < 0)
  {
    return;
  }
  auto Count = Items.size();
  qint64 KeepMs = static_cast<qint64>(KeepDoneFor) * 1000;
  Items.remove_if([KeepMs](const Item & I)
  {
    return I.Finished && (I.Info.State == QueueItemInfo::qsDone) && I.DoneTimer.isValid() && I.DoneTimer.hasExpired(KeepMs);
  });
  if (Count != Items.size())
  {
    emit Q->Changed();
  }
}

void TransferQueue::Private::RefreshOptions(bool All)
{
  Configuration & Config = Configuration::Instance();
  if (All || !LimitExplicit)
  {
    int NewLimit = std::max(1, Config.QueueTransfersLimit);
    if (NewLimit != Limit)
    {
      Q->SetTransfersLimit(NewLimit);
      LimitExplicit = false;
    }
  }
  if (All || !KeepDoneExplicit)
  {
    // TTerminalManager::SetQueueConfiguration
    KeepDoneFor = Config.QueueKeepDoneItems ? Config.QueueKeepDoneItemsFor : 0;
    KeepDoneExplicit = false;
  }
  if (All || !ThresholdExplicit)
  {
    Threshold = Config.ParallelTransferThreshold;
    ThresholdExplicit = false;
  }
  {
    QMutexLocker Lock(&Shared->Mutex);
    Shared->Remember = Config.SessionRememberPassword;
  }
  if (All)
  {
    // hidden settings, kept in WinSCP.ini only (Configuration\Interface)
    IniStorage Storage(&Config.Ini());
    int Reopen = DefaultSessionReopenBackground;
    int Duration = DefaultParallelDurationThreshold;
    if (Storage.OpenSubKey(QStringLiteral("Configuration"), false))
    {
      if (Storage.OpenSubKey(QStringLiteral("Interface"), false))
      {
        Reopen = Storage.ReadInteger(QStringLiteral("SessionReopenBackground"), Reopen);
        Duration = Storage.ReadInteger(QStringLiteral("ParallelDurationThreshold"), Duration);
        Storage.CloseSubKey();
      }
      Storage.CloseSubKey();
    }
    ReopenDelay = std::max(0, Reopen);
    ReopenExplicit = false;
    DurationThreshold = std::max(0, Duration);
    DurationExplicit = false;
  }
}

void TransferQueue::Private::CheckEmpty()
{
  // an error waiting to be acknowledged keeps the queue busy, as in WinSCP
  // (the connection waits in qsError there)
  bool Busy = std::any_of(Items.cbegin(), Items.cend(), [](const Item & I)
  {
    return !I.Deleted && (!I.Finished || (I.Info.State == QueueItemInfo::qsError));
  });
  if (!Busy && WasActive)
  {
    WasActive = false;
    QPointer<TransferQueue> Guard(Q);
    // TCustomScpExplorerForm::UpdateQueueStatus: processing is enabled per transfer batch only
    if (!Configuration::Instance().EnableQueueByDefault && Enabled)
    {
      Enabled = false;
      emit Q->Changed();
    }
    if (Guard)
    {
      emit Q->Emptied();
    }
  }
  else if (Busy)
  {
    WasActive = true;
  }
}

TransferQueue::TransferQueue(const SessionData & Data, UserInterface * UI, QObject * Parent) :
  QObject(Parent),
  d(new Private)
{
  d->Q = this;
  d->Data = Data;
  d->UI = UI;
  d->Shared->Password = Data.Password;
  d->Shared->DataPassword = Data.Password;
  d->RefreshOptions(true);
  // TTerminalManager::NewQueue: Queue->Enabled = WinConfiguration->EnableQueueByDefault
  d->Enabled = Configuration::Instance().EnableQueueByDefault;
  d->Housekeeping.setInterval(HousekeepingIntervalMs);
  connect(&d->Housekeeping, &QTimer::timeout, this, [this]()
  {
    d->RefreshOptions(false);
    d->ExpireDoneItems();
    d->CheckParallel();
  });
  d->Housekeeping.start();
}

TransferQueue::~TransferQueue()
{
  d->Destroying = true;
  d->Housekeeping.stop();
  // Release threads waiting for user actions, cancel all transfers, so the
  // connections close in parallel, then wait for each (Terminal destructor
  // joins its thread). Pending Terminal callbacks die with their Terminal.
  for (Item & I : d->Items)
  {
    d->CancelActions(I);
  }
  QList<Terminal *> Terms;
  for (Worker & W : d->Workers)
  {
    W.UI->Close();
    Terms.append(W.Term);
  }
  for (const QPointer<Terminal> & T : d->Discarded)
  {
    if (T)
    {
      Terms.append(T.data());
    }
  }
  for (Terminal * T : Terms)
  {
    disconnect(T, nullptr, this, nullptr);
    T->Progress().Cancel();
    T->Close();
  }
  for (Terminal * T : Terms)
  {
    delete T;
  }
  d->Workers.clear();
  d->Discarded.clear();
  delete d;
  d = nullptr;
}

static bool CanParallel(const CopyParam & Param, bool Move)
{
  // TTerminal::CanParallel: not for operations needed to be done on a folder
  // after all its files are processed
  return Param.QueueParallel && !Move && (!Param.PreserveTime || !Param.PreserveTimeDirs);
}

static CopyParam QueueCopyParam(const CopyParam & Param)
{
  CopyParam Result = Param;
  // TCustomScpExplorerForm::CopyParamDialog: cpNoConfirmation for background transfers
  if (Result.QueueNoConfirmation)
  {
    Result.ConfirmOverwriting = false;
    Result.ConfirmResume = false;
  }
  return Result;
}

int TransferQueue::AddUpload(const QStringList & LocalPaths, const QString & RemoteDirectory, const CopyParam & Param, bool Move)
{
  Item I;
  I.Info.ToRemote = true;
  I.Info.Move = Move;
  I.Info.Sources = LocalPaths;
  I.Info.TargetDirectory = RemoteDirectory;
  I.Param = QueueCopyParam(Param);
  I.Parallel = CanParallel(Param, Move) && (d->Data.FSProtocol != fsSCPonly);
  return d->Add(std::move(I));
}

int TransferQueue::AddDownload(const QStringList & RemotePaths, const QString & LocalDirectory, const CopyParam & Param, bool Move)
{
  Item I;
  I.Info.ToRemote = false;
  I.Info.Move = Move;
  I.Info.Sources = RemotePaths;
  I.Info.TargetDirectory = LocalDirectory;
  I.Param = QueueCopyParam(Param);
  I.Parallel = CanParallel(Param, Move) && (d->Data.FSProtocol != fsSCPonly);
  return d->Add(std::move(I));
}

int TransferQueue::AddDelete(const QStringList & Paths, bool Remote, bool ToTrash)
{
  Item I;
  I.Info.Type = QueueItemInfo::ikDelete;
  I.Info.ToRemote = Remote;
  I.Info.Sources = Paths;
  I.ToTrash = ToTrash && !Remote;
  return d->Add(std::move(I));
}

QList<QueueItemInfo> TransferQueue::Items() const
{
  QList<QueueItemInfo> Result;
  for (const Item & I : d->Items)
  {
    if (I.Deleted)
    {
      continue;
    }
    QueueItemInfo Info = I.Info;
    if (!I.Finished)
    {
      Info.Progress = d->ItemProgress(I);
      Info.Connections = d->ConnectionsOf(I.Info.Id);
      if (I.Parallel && I.PS && I.PS->Scanned)
      {
        for (const Worker & W : d->Workers)
        {
          if ((W.ItemId == I.Info.Id) && (W.UnitIndex >= 0))
          {
            Info.ConnectionProgress.append(W.Term->Progress().Get());
          }
        }
      }
    }
    Result.append(Info);
  }
  return Result;
}

bool TransferQueue::IsEmpty() const
{
  return std::none_of(d->Items.cbegin(), d->Items.cend(), [](const Item & I) { return !I.Deleted; });
}

int TransferQueue::ActiveCount() const
{
  return static_cast<int>(std::count_if(d->Items.cbegin(), d->Items.cend(),
    [](const Item & I) { return !I.Deleted && !I.Finished; }));
}

int TransferQueue::UserActionItem() const
{
  for (const Item & I : d->Items)
  {
    if (!I.Deleted && (!I.Actions.isEmpty() || (I.Finished && (I.Info.State == QueueItemInfo::qsError))))
    {
      return I.Info.Id;
    }
  }
  return 0;
}

void TransferQueue::Pause(int Id)
{
  Item * I = d->FindItem(Id);
  if ((I == nullptr) || I->Finished || I->Deleted || I->Suspended)
  {
    return;
  }
  I->Suspended = true;
  for (Worker * W : d->WorkersOf(Id))
  {
    if (W->Opened)
    {
      W->Term->Progress().Suspend();
    }
  }
  // while connecting the flag is applied when the transfer starts
  d->UpdateState(*I);
  emit Changed();
}

void TransferQueue::Resume(int Id)
{
  auto It = d->ItemIterator(Id);
  if ((It == d->Items.end()) || It->Finished || It->Deleted)
  {
    return;
  }
  Item & I = *It;
  if (I.Suspended)
  {
    I.Suspended = false;
    for (Worker * W : d->WorkersOf(Id))
    {
      if (W->Opened)
      {
        W->Term->Progress().Resume();
      }
    }
    d->ScheduleProcess();
  }
  else if (!I.Started)
  {
    // TTerminalQueue::ItemExecuteNow
    I.Forced = true;
    d->MoveToFirstPending(It);
    d->ScheduleProcess();
  }
  else
  {
    return;
  }
  d->UpdateState(I);
  emit Changed();
}

void TransferQueue::Delete(int Id)
{
  auto It = d->ItemIterator(Id);
  if ((It == d->Items.end()) || It->Deleted)
  {
    return;
  }
  d->CancelActions(*It);
  QList<Worker *> Ws = d->WorkersOf(Id);
  if (Ws.isEmpty())
  {
    // a failed (or reconnecting) segmented download may have left its part
    // files and the partially merged "<file>.filepart"; no connection writes them now
    if (It->Info.State != QueueItemInfo::qsDone)
    {
      d->CleanupSegments(*It);
    }
    d->Items.erase(It);
  }
  else
  {
    // Removed once the workers report back
    It->Deleted = true;
    for (Worker * W : Ws)
    {
      W->Term->Progress().Cancel();
    }
  }
  emit Changed();
  d->CheckEmpty();
}

void TransferQueue::MoveUp(int Id)
{
  auto It = d->ItemIterator(Id);
  if ((It == d->Items.end()) || It->Started || It->Deleted || (It == d->Items.begin()))
  {
    return;
  }
  auto Prev = std::prev(It);
  while (Prev->Deleted && (Prev != d->Items.begin()))
  {
    --Prev;
  }
  if (Prev->Started || Prev->Deleted)
  {
    return;
  }
  d->Items.splice(Prev, d->Items, It);
  emit Changed();
}

void TransferQueue::MoveDown(int Id)
{
  auto It = d->ItemIterator(Id);
  if ((It == d->Items.end()) || It->Started || It->Deleted)
  {
    return;
  }
  auto Next = std::next(It);
  while ((Next != d->Items.end()) && Next->Deleted)
  {
    ++Next;
  }
  if (Next == d->Items.end())
  {
    return;
  }
  d->Items.splice(std::next(Next), d->Items, It);
  emit Changed();
}

void TransferQueue::PauseAll()
{
  // "Suspend all running queue items"
  bool Any = false;
  for (Item & I : d->Items)
  {
    if (!I.Finished && !I.Deleted && !I.Suspended && (d->ConnectionsOf(I.Info.Id) > 0))
    {
      I.Suspended = true;
      for (Worker * W : d->WorkersOf(I.Info.Id))
      {
        if (W->Opened)
        {
          W->Term->Progress().Suspend();
        }
      }
      d->UpdateState(I);
      Any = true;
    }
  }
  if (Any)
  {
    emit Changed();
  }
}

void TransferQueue::ResumeAll()
{
  bool Any = false;
  for (Item & I : d->Items)
  {
    if (!I.Finished && !I.Deleted && I.Suspended)
    {
      I.Suspended = false;
      for (Worker * W : d->WorkersOf(I.Info.Id))
      {
        if (W->Opened)
        {
          W->Term->Progress().Resume();
        }
      }
      d->UpdateState(I);
      Any = true;
    }
  }
  if (Any)
  {
    d->ScheduleProcess();
    emit Changed();
  }
}

void TransferQueue::DeleteAllDone()
{
  auto Count = d->Items.size();
  d->Items.remove_if([](const Item & I) { return I.Finished && (I.Info.State == QueueItemInfo::qsDone); });
  if (Count != d->Items.size())
  {
    emit Changed();
  }
}

void TransferQueue::DeleteAll()
{
  // TQueueController::ExecuteOperation(qoDeleteAll): every item, running ones are cancelled
  QList<int> Ids;
  for (const Item & I : d->Items)
  {
    if (!I.Deleted)
    {
      Ids.append(I.Info.Id);
    }
  }
  if (Ids.isEmpty())
  {
    return;
  }
  QPointer<TransferQueue> Guard(this);
  for (int Id : Ids)
  {
    if (!Guard)
    {
      return;
    }
    Delete(Id);
  }
}

bool TransferQueue::ProcessUserAction(int Id)
{
  Item * I = d->FindItem(Id);
  if ((I == nullptr) || I->Deleted)
  {
    return false;
  }
  QPointer<TransferQueue> Guard(this);
  if (!I->Actions.isEmpty())
  {
    UserActionPtr Action = I->Actions.first();
    if (Action->Executing)
    {
      return true; // already shown
    }
    bool Gone;
    {
      QMutexLocker Lock(&Action->Mutex);
      Gone = Action->Done;
    }
    if (Gone)
    {
      // the connection gave up waiting meanwhile
      I->Actions.removeAll(Action);
      d->UpdateState(*I);
      emit Changed();
      return true;
    }
    Action->Executing = true;
    Action->Execute(*d->UI);
    Action->Executing = false;
    if (!Guard)
    {
      Action->Complete(false);
      return true;
    }
    I = d->FindItem(Id);
    if (I != nullptr)
    {
      I->Actions.removeAll(Action);
      // Yes to All / No to All answers the other overwrite queries of the item too
      if ((Action->Kind == UserAction::atQuery) && ((Action->Answer == qaYesToAll) || (Action->Answer == qaNoToAll)))
      {
        {
          QMutexLocker Lock(&I->Shared->Mutex);
          I->Shared->OverwriteAll = Action->Answer;
        }
        for (const UserActionPtr & Other : QList<UserActionPtr>(I->Actions))
        {
          if ((Other->Kind == UserAction::atQuery) && ((Other->Answers & Action->Answer) != 0))
          {
            {
              QMutexLocker Lock(&Other->Mutex);
              Other->Answer = Action->Answer;
            }
            I->Actions.removeAll(Other);
            Other->Complete(false);
          }
        }
      }
      d->UpdateState(*I);
    }
    Action->Complete(false);
    emit Changed();
    return true;
  }
  if (I->Finished && (I->Info.State == QueueItemInfo::qsError))
  {
    // TTerminalItem::ProcessEvent: QueryUserException(qaOK | qaCancel); a
    // connection that could not be opened is retried after OK
    bool Retry = I->ConnectFailed;
    QString Message = I->Info.Error;
    QString More = I->Info.MoreMessages;
    QueryUserParams Params;
    Params.Type = qtError;
    unsigned int Answer = d->UI->QueryUserEx(Message, More, Retry ? (qaOK | qaCancel) : qaOK, Params);
    if (!Guard)
    {
      return true;
    }
    auto It = d->ItemIterator(Id);
    if ((It == d->Items.end()) || !It->Finished)
    {
      return true;
    }
    if (Retry && (Answer == qaOK))
    {
      // TTerminalQueue::RetryItem
      It->Finished = false;
      It->Info.Finished = false;
      It->ConnectFailed = false;
      It->Info.Error.clear();
      It->Info.MoreMessages.clear();
      bool Running = It->Parallel && It->PS && It->PS->Scanned;
      It->Started = Running;
      if (!Running)
      {
        d->MoveToFirstPending(It);
      }
      d->WasActive = true;
      d->UpdateState(*It);
      d->ScheduleProcess();
      emit Changed();
    }
    else
    {
      d->CleanupSegments(*It);
      d->Items.erase(It);
      emit Changed();
      if (Guard)
      {
        d->CheckEmpty();
      }
    }
    return true;
  }
  return false;
}

void TransferQueue::SetItemCPSLimit(int Id, unsigned long Limit)
{
  Item * I = d->FindItem(Id);
  if ((I == nullptr) || I->Finished || I->Deleted)
  {
    return;
  }
  // TQueueItem::SetCPSLimit
  I->Info.CPSLimit = Limit;
  I->Param.CPSLimit = Limit;
  d->ApplyCPSLimit(*I);
  emit Changed();
}

QList<QPair<QString, QueueFileState>> TransferQueue::ItemFileList(int Id) const
{
  QList<QPair<QString, QueueFileState>> Result;
  const Item * I = d->FindItem(Id);
  if ((I == nullptr) || !I->PS || !I->PS->Scanned)
  {
    return Result;
  }
  if (I->PS->Segmented)
  {
    bool Processed = std::any_of(I->PS->Units.cbegin(), I->PS->Units.cend(), [](const Unit & U) { return U.Status != Unit::usPending; });
    Result.append(qMakePair(I->PS->Segment ? I->PS->Segment->RemotePath : QString(), Processed ? qfsProcessed : qfsQueued));
    return Result;
  }
  for (const Unit & U : I->PS->Units)
  {
    Result.append(qMakePair(U.Source, (U.Status == Unit::usPending) ? qfsQueued : qfsProcessed));
  }
  return Result;
}

void TransferQueue::SetEnabled(bool Enabled)
{
  if (d->Enabled != Enabled)
  {
    d->Enabled = Enabled;
    if (Enabled)
    {
      d->ScheduleProcess();
    }
    emit Changed();
  }
}

bool TransferQueue::Enabled() const
{
  return d->Enabled;
}

void TransferQueue::SetTransfersLimit(int Limit)
{
  Limit = std::max(1, Limit);
  d->LimitExplicit = true;
  if (d->Limit == Limit)
  {
    return;
  }
  d->Limit = Limit;
  // Drop surplus idle connections; busy ones go when their item is done
  QList<int> Surplus;
  int Count = static_cast<int>(d->Workers.size());
  for (const Worker & W : d->Workers)
  {
    if ((Count - Surplus.size() > Limit) && (W.ItemId == 0))
    {
      Surplus.append(W.Id);
    }
  }
  for (int Id : Surplus)
  {
    d->DiscardWorker(Id);
  }
  d->ScheduleProcess();
}

int TransferQueue::TransfersLimit() const
{
  return d->Limit;
}

void TransferQueue::SetKeepDoneItemsFor(int Seconds)
{
  d->KeepDoneFor = std::max(-1, Seconds);
  d->KeepDoneExplicit = true;
  if (d->KeepDoneFor == 0)
  {
    DeleteAllDone();
  }
}

int TransferQueue::KeepDoneItemsFor() const
{
  return d->KeepDoneFor;
}

void TransferQueue::SetParallelTransferThreshold(int KB)
{
  d->Threshold = KB;
  d->ThresholdExplicit = true;
}

void TransferQueue::SetParallelDurationThreshold(int Seconds)
{
  d->DurationThreshold = std::max(0, Seconds);
  d->DurationExplicit = true;
}

void TransferQueue::SetReopenBackgroundDelay(int Ms)
{
  d->ReopenDelay = std::max(0, Ms);
  d->ReopenExplicit = true;
  for (Worker & W : d->Workers)
  {
    Terminal::Settings Options = W.Term->GetSettings();
    Options.BackgroundReopen = std::max(1, d->ReopenDelay);
    W.Term->SetSettings(Options);
  }
}

void TransferQueue::SetUserActionsDeferred(bool Deferred)
{
  QMutexLocker Lock(&d->Shared->Mutex);
  d->Shared->Deferred = Deferred;
}

void TransferQueue::SetRememberedPassword(const QString & Password, UserInterface::PromptKind Kind)
{
  QMutexLocker Lock(&d->Shared->Mutex);
  if (Kind == UserInterface::pkPassphrase)
  {
    d->Shared->Passphrase = Password;
  }
  else
  {
    d->Shared->Password = Password;
  }
}

void TransferQueue::ApplyConfiguration()
{
  d->RefreshOptions(true);
}
