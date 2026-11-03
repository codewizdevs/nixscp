// Port of WinSCP source/components/DirView.cpp (local), UnixDirView.cpp (remote),
// packages/filemng/CustomDirView.pas (keyboard, history, saved selection),
// packages/filemng/DirViewInt.pas (local listing, change monitoring),
// packages/my/NortonLikeListView.pas (Norton-like marking), packages/my/PathLabel.pas,
// and the panel part of forms/ScpCommander + forms/CustomScpExplorer
// (path combo, history/navigation toolbars, status bar, incremental search,
// drag & drop, column persistence in DirViewParams).
#include "gui/FilePanel.h"
#include "core/Platform.h"
#include "core/LocalPath.h"
#include "gui/Translator.h"
#include "gui/FileListModel.h"
#include "core/Configuration.h"
#include "core/FileMasks.h"
#include "core/IniStorage.h"
#include "core/Terminal.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDir>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMetaMethod>
#include <QMimeData>
#include <QMouseEvent>
#include <QMutex>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QRubberBand>
#include <QScrollBar>
#include <QSet>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QThreadPool>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#ifndef Q_OS_WIN
#include <dirent.h>
#include <fcntl.h>
#include <functional>
#include <grp.h>
#include <pwd.h>
#endif
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

static const QString FilesMimeType = QStringLiteral("application/x-nixscp-files");
static const char * const TerminalHomeProperty = "nixscpHomeDirectory";
static const char * const TerminalRealHomeProperty = "nixscpRealHomeDirectory";
// TCustomDirView DefaultHistoryCount
static const int MaxHistoryCount = 200;
// TTerminalManager DirectoryReadingProgressDelay
static const int ReadingProgressDelay = 500;

//---------------------------------------------------------------------------
// Local listing
//---------------------------------------------------------------------------

namespace
{

#ifndef Q_OS_WIN
// uid/gid -> name, shared by all listings (getpwuid/NSS can be slow, e.g. LDAP)
QMutex NameCacheMutex;
QHash<uint, QString> UserNameCache;
QHash<uint, QString> GroupNameCache;

QString UserNameOf(uid_t Uid)
{
  QMutexLocker Lock(&NameCacheMutex);
  auto It = UserNameCache.constFind(Uid);
  if (It != UserNameCache.constEnd())
  {
    return It.value();
  }
  QString Name = QString::number(Uid);
  struct passwd Pwd;
  struct passwd * Result = nullptr;
  // ERANGE: retry with a bigger buffer
  for (size_t Size = 4096; Size <= 1024 * 1024; Size *= 4)
  {
    std::vector<char> Buffer(Size);
    int Error = getpwuid_r(Uid, &Pwd, Buffer.data(), Buffer.size(), &Result);
    if ((Error == 0) && (Result != nullptr))
    {
      Name = QString::fromLocal8Bit(Result->pw_name);
    }
    if (Error != ERANGE)
    {
      break;
    }
  }
  UserNameCache.insert(Uid, Name);
  return Name;
}

QString GroupNameOf(gid_t Gid)
{
  QMutexLocker Lock(&NameCacheMutex);
  auto It = GroupNameCache.constFind(Gid);
  if (It != GroupNameCache.constEnd())
  {
    return It.value();
  }
  QString Name = QString::number(Gid);
  struct group Grp;
  struct group * Result = nullptr;
  // Groups with many members need big buffers (ERANGE)
  for (size_t Size = 8192; Size <= 4 * 1024 * 1024; Size *= 4)
  {
    std::vector<char> Buffer(Size);
    int Error = getgrgid_r(Gid, &Grp, Buffer.data(), Buffer.size(), &Result);
    if ((Error == 0) && (Result != nullptr))
    {
      Name = QString::fromLocal8Bit(Result->gr_name);
    }
    if (Error != ERANGE)
    {
      break;
    }
  }
  GroupNameCache.insert(Gid, Name);
  return Name;
}
#endif

QString ExpandHome(const QString & Path)
{
  if ((Path == QStringLiteral("~")) || Path.startsWith(QStringLiteral("~/")))
  {
    return QDir::homePath() + Path.mid(1);
  }
  return Path;
}

} // namespace

bool LocalDirectoryProvider::ReadDirectory(const QString & Path, QString & Canonical, RemoteFileList & Files,
  QString & Error, QString & MoreMessages, const std::atomic<bool> * Cancel,
  const std::function<void(int Count)> & Progress)
{
  Files.clear();
  QString Expanded = ExpandHome(Path);
  Error = QStringLiteral("Error listing directory '%1'.").arg(Path);
  QFileInfo DirInfo(Expanded);
  Canonical = DirInfo.canonicalFilePath();
  if (Canonical.isEmpty() || !DirInfo.isDir())
  {
    MoreMessages = DirInfo.exists() ? QStringLiteral("Not a directory.") : QStringLiteral("No such file or directory.");
    Canonical.clear();
    return false;
  }
#ifdef Q_OS_WIN
  // One FindFirstFileExW call returns name, size, time and attributes of each entry
  std::wstring Pattern = QDir::toNativeSeparators(Canonical + QStringLiteral("/*")).toStdWString();
  WIN32_FIND_DATAW Data;
  HANDLE Find = FindFirstFileExW(Pattern.c_str(), FindExInfoBasic, &Data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
  if (Find == INVALID_HANDLE_VALUE)
  {
    if (GetLastError() == ERROR_FILE_NOT_FOUND)
    {
      Error.clear();
      return true;
    }
    MoreMessages = Platform::LastErrorText();
    return false;
  }
  QElapsedTimer ProgressTimer;
  ProgressTimer.start();
  do
  {
    QString Name = QString::fromWCharArray(Data.cFileName);
    if ((Name == QStringLiteral(".")) || (Name == QStringLiteral("..")))
    {
      continue;
    }
    if ((Cancel != nullptr) && Cancel->load())
    {
      FindClose(Find);
      Files.clear();
      MoreMessages = QStringLiteral("Operation was aborted.");
      return false;
    }
    RemoteFile File;
    File.FileName = Name;
    DWORD Attributes = Data.dwFileAttributes;
    // Symbolic links and junctions (dwReserved0 is the reparse tag)
    File.IsSymLink = (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
      ((Data.dwReserved0 == IO_REPARSE_TAG_SYMLINK) || (Data.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT));
    if (File.IsSymLink)
    {
      File.LinkTo = QFileInfo(Canonical + QLatin1Char('/') + Name).symLinkTarget();
    }
    File.IsDirectory = (Attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    File.FileType = File.IsSymLink ? RemoteFile::Symlink : (File.IsDirectory ? RemoteFile::Directory : RemoteFile::File);
    File.Size = File.IsDirectory ? 0 : ((static_cast<qint64>(Data.nFileSizeHigh) << 32) | Data.nFileSizeLow);
    ULARGE_INTEGER Time;
    Time.LowPart = Data.ftLastWriteTime.dwLowDateTime;
    Time.HighPart = Data.ftLastWriteTime.dwHighDateTime;
    // 100 ns intervals since 1601-01-01
    File.Modification = QDateTime::fromMSecsSinceEpoch((static_cast<qint64>(Time.QuadPart) - 116444736000000000LL) / 10000);
    File.Permissions = File.IsDirectory ? 0777 : ((Attributes & FILE_ATTRIBUTE_READONLY) ? 0444 : 0666);
    File.IsHidden = (Attributes & FILE_ATTRIBUTE_HIDDEN) || Name.startsWith(QLatin1Char('.'));
    Files.append(File);
    if (Progress && ((Files.size() & 0xFF) == 0) && (ProgressTimer.elapsed() >= 100))
    {
      Progress(Files.size());
      ProgressTimer.restart();
    }
  }
  while (FindNextFileW(Find, &Data));
  FindClose(Find);
  Error.clear();
  return true;
#else
  QByteArray NativePath = QFile::encodeName(Canonical);
  if (::access(NativePath.constData(), R_OK | X_OK) != 0)
  {
    MoreMessages = QString::fromLocal8Bit(std::strerror(errno));
    return false;
  }
  DIR * Dir = ::opendir(NativePath.constData());
  if (Dir == nullptr)
  {
    MoreMessages = QString::fromLocal8Bit(std::strerror(errno));
    return false;
  }
  int Fd = ::dirfd(Dir);
  QElapsedTimer ProgressTimer;
  ProgressTimer.start();
  struct dirent * Entry;
  while ((Entry = ::readdir(Dir)) != nullptr)
  {
    const char * Name = Entry->d_name;
    if ((std::strcmp(Name, ".") == 0) || (std::strcmp(Name, "..") == 0))
    {
      continue;
    }
    if ((Cancel != nullptr) && Cancel->load())
    {
      ::closedir(Dir);
      Files.clear();
      MoreMessages = QStringLiteral("Operation was aborted.");
      return false;
    }
    struct stat LinkStat;
    if (::fstatat(Fd, Name, &LinkStat, AT_SYMLINK_NOFOLLOW) != 0)
    {
      continue;   // removed meanwhile
    }
    RemoteFile File;
    File.FileName = QFile::decodeName(QByteArray(Name));
    File.IsSymLink = S_ISLNK(LinkStat.st_mode);
    const struct stat * St = &LinkStat;
    struct stat TargetStat;
    if (File.IsSymLink)
    {
      if (::fstatat(Fd, Name, &TargetStat, 0) == 0)
      {
        St = &TargetStat;
      }
      char Target[PATH_MAX];
      ssize_t Length = ::readlinkat(Fd, Name, Target, sizeof(Target) - 1);
      if (Length >= 0)
      {
        File.LinkTo = QFile::decodeName(QByteArray(Target, static_cast<int>(Length)));
      }
    }
    File.IsDirectory = S_ISDIR(St->st_mode);   // follows symlinks
    File.FileType = File.IsSymLink ? RemoteFile::Symlink :
      (File.IsDirectory ? RemoteFile::Directory : (S_ISREG(St->st_mode) ? RemoteFile::File : RemoteFile::Other));
    File.Size = File.IsDirectory ? 0 : static_cast<qint64>(St->st_size);
    File.Modification = QDateTime::fromMSecsSinceEpoch(
      static_cast<qint64>(St->NIXSCP_ST_MTIM.tv_sec) * 1000 + St->NIXSCP_ST_MTIM.tv_nsec / 1000000);
    File.Permissions = St->st_mode & 07777;
    File.IsHidden = (Name[0] == '.');
    File.Owner = UserNameOf(St->st_uid);
    File.Group = GroupNameOf(St->st_gid);
    Files.append(File);
    if (Progress && ((Files.size() & 0xFF) == 0) && (ProgressTimer.elapsed() >= 100))
    {
      Progress(Files.size());
      ProgressTimer.restart();
    }
  }
  ::closedir(Dir);
  Error.clear();
  return true;
#endif
}

LocalDirectoryProvider::~LocalDirectoryProvider()
{
  if (FCancel)
  {
    FCancel->store(true);
  }
}

void LocalDirectoryProvider::Load(const QString & Path, LoadCallback Callback)
{
  LoadWithProgress(Path, ProgressCallback(), Callback);
}

void LocalDirectoryProvider::LoadWithProgress(const QString & Path, ProgressCallback Progress, LoadCallback Callback)
{
  if (Synchronous)
  {
    QString Canonical, Error, More;
    RemoteFileList Files;
    bool Ok = ReadDirectory(Path, Canonical, Files, Error, More);
    Callback(Ok, Canonical, Files, Error, More);
    return;
  }
  // A newer listing supersedes the running one
  CancelLoad();
  auto Cancel = std::make_shared<std::atomic<bool>>(false);
  FCancel = Cancel;
  // Own pool, never destroyed: a listing stuck on a stale mount must neither
  // block other thread pool users nor the application exit
  static QThreadPool * Pool = []()
  {
    QThreadPool * Result = new QThreadPool();
    Result->setMaxThreadCount(4);
    return Result;
  }();
  Pool->start([Path, Progress, Callback, Cancel]()
  {
    std::function<void(int)> Report;
    if (Progress)
    {
      Report = [Progress, Cancel](int Count)
      {
        QMetaObject::invokeMethod(qApp, [Progress, Cancel, Count]()
        {
          if (!Cancel->load())
          {
            Progress(Count);
          }
        }, Qt::QueuedConnection);
      };
    }
    QString Canonical, Error, More;
    RemoteFileList Files;
    bool Ok = ReadDirectory(Path, Canonical, Files, Error, More, Cancel.get(), Report);
    if (Cancel->load() || (qApp == nullptr))
    {
      return;
    }
    QMetaObject::invokeMethod(qApp, [Callback, Cancel, Ok, Canonical, Files, Error, More]()
    {
      if (!Cancel->load())
      {
        Callback(Ok, Canonical, Files, Error, More);
      }
    }, Qt::QueuedConnection);
  });
}

void LocalDirectoryProvider::CancelLoad()
{
  if (FCancel)
  {
    FCancel->store(true);
    FCancel.reset();
  }
}

QString LocalDirectoryProvider::HomeDirectory() const
{
  return QDir::homePath();
}

//---------------------------------------------------------------------------
// Remote listing
//---------------------------------------------------------------------------

RemoteDirectoryProvider::RemoteDirectoryProvider(Terminal * ATerminal) :
  FTerminal(ATerminal)
{
}

void RemoteDirectoryProvider::Load(const QString & Path, LoadCallback Callback)
{
  if (FTerminal == nullptr)
  {
    Callback(false, QString(), RemoteFileList(), QStringLiteral("Not connected."), QString());
    return;
  }
  // Remember the initial (login) directory as home, before the first listing changes it
  if (!FTerminal->property(TerminalHomeProperty).isValid() && !FTerminal->CurrentDirectory().isEmpty())
  {
    FTerminal->setProperty(TerminalHomeProperty, FTerminal->CurrentDirectory());
  }
  FTerminal->ReadDirectory(Path,
    [Callback](const OperationResult & Result, const QString & Directory, const RemoteFileList & Files)
    {
      if (Result.Ok)
      {
        Callback(true, Directory, Files, QString(), QString());
      }
      else
      {
        QString Message = Result.Message;
        if (Message.isEmpty())
        {
          Message = Result.Aborted ? QStringLiteral("Operation was aborted.") : QStringLiteral("Error listing directory.");
        }
        Callback(false, Directory, RemoteFileList(), Message, Result.MoreMessages);
      }
    });
}

QString RemoteDirectoryProvider::HomeDirectory() const
{
  if (FTerminal == nullptr)
  {
    return QStringLiteral("/");
  }
  QVariant Home = FTerminal->property(TerminalHomeProperty);
  if (Home.isValid() && !Home.toString().isEmpty())
  {
    return Home.toString();
  }
  QString Current = FTerminal->CurrentDirectory();
  return Current.isEmpty() ? QStringLiteral("/") : Current;
}

void RemoteDirectoryProvider::ResolveHomeDirectory(std::function<void(const QString & Path)> Done)
{
  if (FTerminal == nullptr)
  {
    Done(QStringLiteral("/"));
    return;
  }
  const SessionData & Data = FTerminal->Data();
  QString SessionDirectory = Data.RemoteDirectory;
  bool UseSessionDirectory =
    Configuration::Instance().DefaultDirIsHome && !SessionDirectory.isEmpty() && !Data.UpdateDirectories;
  if (UseSessionDirectory && SessionDirectory.startsWith(QLatin1Char('/')))
  {
    Done(SessionDirectory);
    return;
  }
  auto Finish = [UseSessionDirectory, SessionDirectory, Done](const QString & Home)
  {
    Done(UseSessionDirectory ? UnixPath::Combine(Home, SessionDirectory) : Home);
  };
  QVariant RealHome = FTerminal->property(TerminalRealHomeProperty);
  if (RealHome.isValid() && !RealHome.toString().isEmpty())
  {
    Finish(RealHome.toString());
    return;
  }
  if (SessionDirectory.isEmpty())
  {
    // The session started in the home directory
    Finish(HomeDirectory());
    return;
  }
  // HOMEDIRECTORY: ask the server
  auto Result = std::make_shared<QString>();
  QPointer<Terminal> Term(FTerminal);
  QString Fallback = HomeDirectory();
  FTerminal->RunOnWorker([Result](FileSystem & Fs, FileOperationProgress &, UserInterface &)
    {
      *Result = Fs.HomeDirectory();
    },
    [Term, Result, Finish, Fallback](const OperationResult & Operation)
    {
      if (Operation.Ok && !Result->isEmpty())
      {
        if (!Term.isNull())
        {
          Term->setProperty(TerminalRealHomeProperty, *Result);
        }
        Finish(*Result);
      }
      else
      {
        Finish(Fallback);
      }
    });
}

bool RemoteDirectoryProvider::IsBusy() const
{
  return (FTerminal != nullptr) && FTerminal->Busy();
}

QString RemoteDirectoryProvider::UserName() const
{
  return (FTerminal != nullptr) ? FTerminal->Data().UserName : QString();
}

//---------------------------------------------------------------------------
// View, delegate, path label
//---------------------------------------------------------------------------

namespace
{

struct DropData
{
  bool Valid = false;
  bool FromRemote = false;
  bool OwnFiles = false;          // dragged from the same panel
  QStringList Paths;
};

// Implemented by FilePanel::Private: keyboard/mouse selection logic of
// TCustomNortonLikeListView / TCustomDirView
class PanelViewHandler
{
public:
  virtual ~PanelViewHandler() = default;
  virtual bool ViewKeyPress(QKeyEvent * Event) = 0;
  virtual bool ViewMousePress(QMouseEvent * Event) = 0;
  virtual bool ViewMouseMove(QMouseEvent * Event) = 0;
  virtual bool ViewMouseRelease(QMouseEvent * Event) = 0;
  virtual bool ViewMouseDoubleClick(QMouseEvent * Event) = 0;
  virtual QMimeData * ViewCreateDragData() = 0;
  virtual void ViewDragFinished() = 0;
  virtual bool ViewIsDropTargetRow(int Row) = 0;
  virtual bool ViewIsRowDragged(int Row) = 0;
  virtual void ViewDrop(const DropData & Data, int Row, Qt::DropAction Action) = 0;
  virtual bool ViewIsMarked(int Row) = 0;
  virtual bool ViewFullRowSelect() = 0;
};

class PanelView : public QTreeView
{
public:
  explicit PanelView(QWidget * Parent) : QTreeView(Parent) {}

  PanelViewHandler * Handler = nullptr;

  // Key: Qt::Key_Up/Down/PageUp/PageDown/Home/End
  QModelIndex MoveCursorPublic(int Key)
  {
    CursorAction Action;
    switch (Key)
    {
      case Qt::Key_Up: Action = MoveUp; break;
      case Qt::Key_Down: Action = MoveDown; break;
      case Qt::Key_PageUp: Action = MovePageUp; break;
      case Qt::Key_PageDown: Action = MovePageDown; break;
      case Qt::Key_Home: Action = MoveHome; break;
      case Qt::Key_End: Action = MoveEnd; break;
      default: return QModelIndex();
    }
    return moveCursor(Action, Qt::NoModifier);
  }
  void StartDragPublic()
  {
    startDrag(Qt::CopyAction | Qt::MoveAction);
  }
  bool IsEditing() const
  {
    return state() == QAbstractItemView::EditingState;
  }
  QPoint ContentOffset() const
  {
    return QPoint(horizontalOffset(), verticalOffset());
  }

protected:
  void keyPressEvent(QKeyEvent * Event) override
  {
    if ((Handler != nullptr) && Handler->ViewKeyPress(Event))
    {
      Event->accept();
      return;
    }
    QTreeView::keyPressEvent(Event);
  }

  void mousePressEvent(QMouseEvent * Event) override
  {
    if ((Handler != nullptr) && Handler->ViewMousePress(Event))
    {
      Event->accept();
      return;
    }
    QTreeView::mousePressEvent(Event);
  }

  void mouseMoveEvent(QMouseEvent * Event) override
  {
    if ((Handler != nullptr) && Handler->ViewMouseMove(Event))
    {
      Event->accept();
      return;
    }
    QTreeView::mouseMoveEvent(Event);
  }

  void mouseReleaseEvent(QMouseEvent * Event) override
  {
    if ((Handler != nullptr) && Handler->ViewMouseRelease(Event))
    {
      Event->accept();
      return;
    }
    QTreeView::mouseReleaseEvent(Event);
  }

  void mouseDoubleClickEvent(QMouseEvent * Event) override
  {
    if ((Handler != nullptr) && Handler->ViewMouseDoubleClick(Event))
    {
      Event->accept();
      return;
    }
    QTreeView::mouseDoubleClickEvent(Event);
  }

  void startDrag(Qt::DropActions SupportedActions) override
  {
    if (Handler == nullptr)
    {
      return;
    }
    QMimeData * Data = Handler->ViewCreateDragData();
    if (Data == nullptr)
    {
      Handler->ViewDragFinished();
      return;
    }
    QDrag * Drag = new QDrag(this);
    Drag->setMimeData(Data);
    QModelIndex Current = currentIndex();
    if (Current.isValid())
    {
      QIcon Icon = model()->index(Current.row(), 0).data(Qt::DecorationRole).value<QIcon>();
      if (!Icon.isNull())
      {
        int Size = style()->pixelMetric(QStyle::PM_LargeIconSize, nullptr, this);
        Drag->setPixmap(Icon.pixmap(Size, Size));
      }
    }
    Drag->exec(SupportedActions & (Qt::CopyAction | Qt::MoveAction), Qt::CopyAction);
    Handler->ViewDragFinished();
  }

  void dragEnterEvent(QDragEnterEvent * Event) override
  {
    DropData Data = Decode(Event);
    if (Data.Valid)
    {
      Event->setDropAction(ActionFor(Event, Data));
      Event->accept();
    }
    else
    {
      Event->ignore();
    }
  }

  void dragMoveEvent(QDragMoveEvent * Event) override
  {
    // Base class handles auto-scroll; acceptance is decided here
    QTreeView::dragMoveEvent(Event);
    DropData Data = Decode(Event);
    int Row = TargetRow(Event->position().toPoint());
    if (Data.Valid && (!Data.OwnFiles || (Row >= 0)))
    {
      SetDropRow(Row);
      Event->setDropAction(ActionFor(Event, Data));
      Event->accept();
    }
    else
    {
      SetDropRow(-1);
      Event->ignore();
    }
  }

  void dragLeaveEvent(QDragLeaveEvent * Event) override
  {
    SetDropRow(-1);
    QTreeView::dragLeaveEvent(Event);
  }

  void drawRow(QPainter * Painter, const QStyleOptionViewItem & Option, const QModelIndex & Index) const override
  {
    if (Index.row() == FDropRow)
    {
      // Highlights the directory the files would be dropped into (DirView drop target)
      QStyleOptionViewItem Highlighted = Option;
      Highlighted.state |= QStyle::State_Selected | QStyle::State_MouseOver;
      QTreeView::drawRow(Painter, Highlighted, Index);
      Painter->save();
      Painter->setPen(QPen(palette().color(QPalette::Highlight), 2));
      Painter->drawRect(Option.rect.adjusted(1, 1, -1, -1));
      Painter->restore();
      return;
    }
    QTreeView::drawRow(Painter, Option, Index);
    // Focused item (the Norton-like cursor), independent of the marks
    if ((Handler != nullptr) && hasFocus() && (Index.row() == currentIndex().row()) && !IsEditing())
    {
      QRect Rect = Option.rect;
      if (!Handler->ViewFullRowSelect())
      {
        Rect = QRect(columnViewportPosition(FileListModel::ColName), Rect.top(),
          columnWidth(FileListModel::ColName), Rect.height());
      }
      bool Marked = Handler->ViewIsMarked(Index.row());
      Painter->save();
      QPen Pen(palette().color(Marked ? QPalette::HighlightedText : QPalette::Highlight));
      Pen.setStyle(Marked ? Qt::DotLine : Qt::SolidLine);
      Painter->setPen(Pen);
      Painter->setBrush(Qt::NoBrush);
      Painter->drawRect(Rect.adjusted(0, 0, -1, -1));
      Painter->restore();
    }
  }

  void dropEvent(QDropEvent * Event) override
  {
    stopAutoScroll();
    setState(QAbstractItemView::NoState);
    SetDropRow(-1);
    viewport()->update();
    DropData Data = Decode(Event);
    int Row = TargetRow(Event->position().toPoint());
    if (!Data.Valid || (Data.OwnFiles && (Row < 0)))
    {
      Event->ignore();
      return;
    }
    Qt::DropAction Action = ActionFor(Event, Data);
    Event->setDropAction(Action);
    Event->accept();
    if (Handler != nullptr)
    {
      Handler->ViewDrop(Data, Row, Action);
    }
  }

private:
  int FDropRow = -1;

  // As WinSCP/Explorer: within the same panel the default is move, between
  // panels (and from other applications) copy; Ctrl forces copy, Shift forces move
  static Qt::DropAction ActionFor(const QDropEvent * Event, const DropData & Data)
  {
    Qt::KeyboardModifiers Mods = Event->modifiers();
    if (Mods & Qt::ControlModifier) return Qt::CopyAction;
    if (Mods & Qt::ShiftModifier) return Qt::MoveAction;
    return Data.OwnFiles ? Qt::MoveAction : Qt::CopyAction;
  }

  void SetDropRow(int Row)
  {
    if (Row != FDropRow)
    {
      FDropRow = Row;
      viewport()->update();
    }
  }

  // Directory row under the cursor that accepts the drop, or -1 (= current directory)
  int TargetRow(const QPoint & Pos) const
  {
    QModelIndex Index = indexAt(Pos);
    if (!Index.isValid() || (Handler == nullptr) || !Handler->ViewIsDropTargetRow(Index.row()))
    {
      return -1;
    }
    if (Handler->ViewIsRowDragged(Index.row()))
    {
      return -1;
    }
    return Index.row();
  }

  DropData Decode(const QDropEvent * Event) const
  {
    DropData Result;
    const QMimeData * Mime = Event->mimeData();
    if (Mime == nullptr)
    {
      return Result;
    }
    if (Mime->hasFormat(FilesMimeType))
    {
      QJsonObject Obj = QJsonDocument::fromJson(Mime->data(FilesMimeType)).object();
      Result.FromRemote = Obj.value(QStringLiteral("remote")).toBool();
      for (const QJsonValue & Value : Obj.value(QStringLiteral("paths")).toArray())
      {
        Result.Paths.append(Value.toString());
      }
      Result.OwnFiles = (Event->source() == this);
      Result.Valid = !Result.Paths.isEmpty();
    }
    else if (Mime->hasUrls())
    {
      for (const QUrl & Url : Mime->urls())
      {
        if (Url.isLocalFile())
        {
          Result.Paths.append(QDir::cleanPath(Url.toLocalFile()));
        }
      }
      Result.FromRemote = false;
      Result.Valid = !Result.Paths.isEmpty();
    }
    return Result;
  }
};

// Paints marks (selection) and overlay icons; selects the base name for renaming
class PanelDelegate : public QStyledItemDelegate
{
public:
  PanelDelegate(FileListModel * AModel, QObject * Parent) : QStyledItemDelegate(Parent), FModel(AModel) {}

  bool FullRowSelect = false;     // FullRowSelect
  bool RenameWholeName = false;   // RenameWholeName

  void paint(QPainter * Painter, const QStyleOptionViewItem & Option, const QModelIndex & Index) const override
  {
    QStyleOptionViewItem Opt = Option;
    bool Marked = Index.data(FileListModel::MarkedRole).toBool();
    // Without full row select only the name is highlighted (list view label)
    Opt.state.setFlag(QStyle::State_Selected, Marked && (FullRowSelect || (Index.column() == FileListModel::ColName)));
    Opt.state &= ~QStyle::State_HasFocus;
    QStyledItemDelegate::paint(Painter, Opt, Index);
    if (Index.column() == FileListModel::ColName)
    {
      int Overlays = Index.data(FileListModel::OverlayRole).toInt();
      if (Overlays != FileListModel::ovNone)
      {
        PaintOverlays(Painter, Opt, Index, Overlays);
      }
    }
  }

  void setEditorData(QWidget * Editor, const QModelIndex & Index) const override
  {
    QStyledItemDelegate::setEditorData(Editor, Index);
    QLineEdit * Edit = qobject_cast<QLineEdit *>(Editor);
    if ((Edit == nullptr) || RenameWholeName || FModel->File(Index.row()).IsDirectory)
    {
      return;
    }
    // EditSelectBaseName: the view selects all after this, so select later
    // (WinSCP posts EM_SETSEL for the same reason)
    int Dot = Edit->text().lastIndexOf(QLatin1Char('.'));
    if (Dot > 0)
    {
      QPointer<QLineEdit> Guard(Edit);
      QTimer::singleShot(0, Edit, [Guard, Dot]()
      {
        if (!Guard.isNull())
        {
          Guard->setSelection(0, Dot);
        }
      });
    }
  }

private:
  FileListModel * FModel;

  static QIcon EmblemIcon(int Overlay)
  {
    static QIcon Link = QIcon::fromTheme(QStringLiteral("emblem-symbolic-link"));
    static QIcon Broken = QIcon::fromTheme(QStringLiteral("emblem-unreadable"));
    static QIcon Partial = QIcon::fromTheme(QStringLiteral("emblem-downloads"),
      QIcon::fromTheme(QStringLiteral("emblem-synchronizing")));
    switch (Overlay)
    {
      case FileListModel::ovLink: return Link;
      case FileListModel::ovBrokenLink: return Broken;
      case FileListModel::ovPartial: return Partial;
    }
    return QIcon();
  }

  void PaintOverlays(QPainter * Painter, const QStyleOptionViewItem & Option, const QModelIndex & Index, int Overlays) const
  {
    QStyleOptionViewItem Opt = Option;
    initStyleOption(&Opt, Index);
    const QWidget * Widget = Option.widget;
    QStyle * Style = (Widget != nullptr) ? Widget->style() : QApplication::style();
    QRect IconRect = Style->subElementRect(QStyle::SE_ItemViewItemDecoration, &Opt, Widget);
    if (!IconRect.isValid())
    {
      return;
    }
    int Size = std::max(8, IconRect.height() / 2 + 1);
    QRect Rect(IconRect.left(), IconRect.bottom() - Size + 1, Size, Size);
    Painter->save();
    for (int Overlay : { static_cast<int>(FileListModel::ovLink), static_cast<int>(FileListModel::ovBrokenLink),
                         static_cast<int>(FileListModel::ovPartial) })
    {
      if ((Overlays & Overlay) == 0)
      {
        continue;
      }
      QIcon Emblem = EmblemIcon(Overlay);
      if (!Emblem.isNull())
      {
        Emblem.paint(Painter, Rect);
      }
      else
      {
        // Shortcut-like arrow (red for broken links, blue for partial files)
        QColor Color = (Overlay == FileListModel::ovBrokenLink) ? QColor(200, 0, 0) :
          ((Overlay == FileListModel::ovPartial) ? QColor(0, 90, 200) : QColor(0, 0, 0));
        Painter->setRenderHint(QPainter::Antialiasing, true);
        Painter->setPen(QPen(Qt::white, 1));
        Painter->setBrush(Qt::white);
        Painter->drawRect(Rect.adjusted(0, 0, -1, -1));
        Painter->setPen(QPen(Color, std::max(1.0, Size / 7.0)));
        QPointF From(Rect.left() + Size * 0.25, Rect.bottom() - Size * 0.2);
        QPointF To(Rect.right() - Size * 0.2, Rect.top() + Size * 0.25);
        Painter->drawLine(From, To);
        Painter->drawLine(To, QPointF(To.x() - Size * 0.35, To.y()));
        Painter->drawLine(To, QPointF(To.x(), To.y() + Size * 0.35));
      }
    }
    Painter->restore();
  }
};

// Remote path combo drop-down: path components from the root, indented
// (TCustomScpExplorerForm::UpdateRemotePathComboBox)
class PathComboDelegate : public QStyledItemDelegate
{
public:
  using QStyledItemDelegate::QStyledItemDelegate;
  static const int LevelRole = Qt::UserRole + 10;

  void paint(QPainter * Painter, const QStyleOptionViewItem & Option, const QModelIndex & Index) const override
  {
    QStyleOptionViewItem Opt = Option;
    int Level = Index.data(LevelRole).toInt();
    Opt.rect.setLeft(Opt.rect.left() + Level * 10);
    QStyledItemDelegate::paint(Painter, Opt, Index);
  }

protected:
  void initStyleOption(QStyleOptionViewItem * Option, const QModelIndex & Index) const override
  {
    QStyledItemDelegate::initStyleOption(Option, Index);
    if (Index.data(LevelRole).isValid())
    {
      QString Path = Index.data(Qt::DisplayRole).toString();
      QString Name = UnixPath::ExtractFileName(Path);
      Option->text = Name.isEmpty() ? Path : Name;
    }
  }
};

// TPathLabel: current path with clickable components, filter mask at the end;
// colored as the active/inactive caption
class PathLabel : public QWidget
{
public:
  explicit PathLabel(QWidget * Parent) : QWidget(Parent)
  {
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setCursor(Qt::ArrowCursor);
  }

  std::function<void(const QString & Path)> OnPathClick;
  std::function<void()> OnMaskClick;
  std::function<void()> OnClick;
  std::function<void()> OnDoubleClick;
  std::function<void(const QPoint & GlobalPos)> OnContextMenu;

  void SetPath(const QString & Path)
  {
    if (FPath != Path)
    {
      FPath = Path;
      update();
    }
  }
  void SetMask(const QString & Mask)
  {
    if (FMask != Mask)
    {
      FMask = Mask;
      update();
    }
  }
  void SetActive(bool Active)
  {
    if (FActive != Active)
    {
      FActive = Active;
      update();
    }
  }

  QSize sizeHint() const override
  {
    return QSize(100, fontMetrics().height() + 4);
  }
  QSize minimumSizeHint() const override
  {
    return QSize(20, fontMetrics().height() + 4);
  }

protected:
  struct Segment
  {
    QString Text;
    QString Path;     // empty for the mask
    bool Mask = false;
    int Left = 0;
    int Width = 0;
  };

  QList<Segment> Segments() const
  {
    QList<Segment> Result;
    if (FPath.isEmpty())
    {
      return Result;
    }
    QFontMetrics Metrics(font());
    // Components: "/", "home/", "user/", "dir" (Windows local: "C:\", "Users\"...)
    QList<Segment> Parts;
    Segment Root;
    QString RootPath = QStringLiteral("/");
    QChar Separator = QLatin1Char('/');
#ifdef Q_OS_WIN
    // Remote paths always start with "/"; local ones with a drive or a UNC share
    if (!FPath.startsWith(QLatin1Char('/')) || FPath.startsWith(QStringLiteral("//")))
    {
      RootPath = LocalPath::Root(FPath);
      Separator = QLatin1Char('\\');
    }
#endif
    Root.Text = (Separator == QLatin1Char('/')) ? RootPath : LocalPath::Display(RootPath);
    Root.Path = RootPath;
    Parts.append(Root);
    QStringList Names = FPath.mid(RootPath.size()).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString Current = RootPath;
    for (int Index = 0; Index < Names.size(); Index++)
    {
      Current = (Index == 0) ? (RootPath + Names.at(Index)) : (Current + QLatin1Char('/') + Names.at(Index));
      Segment Part;
      Part.Text = Names.at(Index) + Separator;
      Part.Path = Current;
      Parts.append(Part);
    }
    // The mask follows the path ("*.*" while hovering when there is no filter)
    QString MaskText = FMask.isEmpty() ? (FHover ? QStringLiteral("*.*") : QString()) : FMask;
    int Available = width() - 8;
    int MaskWidth = MaskText.isEmpty() ? 0 : Metrics.horizontalAdvance(MaskText);
    if (MaskWidth > Available / 3)
    {
      MaskWidth = Available / 3;
    }
    int PathAvailable = Available - MaskWidth;
    auto TotalWidth = [&Metrics](const QList<Segment> & List)
    {
      int Sum = 0;
      for (const Segment & S : List)
      {
        Sum += Metrics.horizontalAdvance(S.Text);
      }
      return Sum;
    };
    // Too long: "/.../user/dir"
    if ((TotalWidth(Parts) > PathAvailable) && (Parts.size() > 2))
    {
      Segment Ellipsis;
      Ellipsis.Text = QStringLiteral("…/");
      while ((Parts.size() > 2) && (TotalWidth(Parts) + Metrics.horizontalAdvance(Ellipsis.Text) > PathAvailable))
      {
        Ellipsis.Path = Parts.at(1).Path;
        Parts.removeAt(1);
      }
      Parts.insert(1, Ellipsis);
    }
    int Left = 4;
    for (Segment & Part : Parts)
    {
      Part.Left = Left;
      Part.Width = Metrics.horizontalAdvance(Part.Text);
      Left += Part.Width;
      Result.append(Part);
    }
    if (!MaskText.isEmpty())
    {
      Segment MaskSegment;
      MaskSegment.Text = Metrics.elidedText(MaskText, Qt::ElideRight, std::max(MaskWidth, 10));
      MaskSegment.Mask = true;
      MaskSegment.Left = Left;
      MaskSegment.Width = Metrics.horizontalAdvance(MaskSegment.Text);
      Result.append(MaskSegment);
    }
    return Result;
  }

  int SegmentAt(const QList<Segment> & List, int X) const
  {
    for (int Index = 0; Index < List.size(); Index++)
    {
      if ((X >= List.at(Index).Left) && (X < List.at(Index).Left + List.at(Index).Width))
      {
        return Index;
      }
    }
    return -1;
  }

  void paintEvent(QPaintEvent *) override
  {
    QPainter Painter(this);
    QPalette Pal = palette();
    QColor Background = FActive ? Pal.color(QPalette::Highlight) : Pal.color(QPalette::Mid);
    QColor Text = FActive ? Pal.color(QPalette::HighlightedText) : Pal.color(QPalette::Window);
    if (!FActive)
    {
      // Unreadable on the background (light Mid, or the dark theme where
      // Window is dark too): WindowText instead
      auto Distance = [&Background](const QColor & Color) { return std::abs(qGray(Color.rgb()) - qGray(Background.rgb())); };
      if ((Distance(Text) < 64) && (Distance(Pal.color(QPalette::WindowText)) > Distance(Text)))
      {
        Text = Pal.color(QPalette::WindowText);
      }
    }
    Painter.fillRect(rect(), Background);
    const QList<Segment> List = Segments();
    QFont Normal = font();
    QFont Hot = font();
    Hot.setUnderline(true);
    int Baseline = (height() + fontMetrics().ascent() - fontMetrics().descent()) / 2;
    // Hot tracking: the hovered component and everything before it
    int HotIndex = FHover ? SegmentAt(List, FMouseX) : -1;
    for (int Index = 0; Index < List.size(); Index++)
    {
      const Segment & S = List.at(Index);
      bool IsHot = (HotIndex >= 0) && (List.at(HotIndex).Mask ? (Index == HotIndex) : (!S.Mask && (Index <= HotIndex)));
      Painter.setFont(IsHot ? Hot : Normal);
      QColor Color = Text;
      if (S.Mask && FMask.isEmpty())
      {
        Color.setAlpha(150);
      }
      Painter.setPen(Color);
      Painter.drawText(S.Left, Baseline, S.Text);
    }
  }

  void mouseMoveEvent(QMouseEvent * Event) override
  {
    FHover = true;
    FMouseX = static_cast<int>(Event->position().x());
    const QList<Segment> List = Segments();
    setCursor((SegmentAt(List, FMouseX) >= 0) ? Qt::PointingHandCursor : Qt::ArrowCursor);
    update();
  }

  void enterEvent(QEnterEvent * Event) override
  {
    FHover = true;
    FMouseX = static_cast<int>(Event->position().x());
    update();
  }

  void leaveEvent(QEvent *) override
  {
    FHover = false;
    update();
  }

  void mouseDoubleClickEvent(QMouseEvent * Event) override
  {
    if ((Event->button() == Qt::LeftButton) && OnDoubleClick)
    {
      OnDoubleClick();
    }
  }

  void contextMenuEvent(QContextMenuEvent * Event) override
  {
    if (OnContextMenu)
    {
      OnContextMenu(Event->globalPos());
    }
  }

  void mousePressEvent(QMouseEvent * Event) override
  {
    if (Event->button() != Qt::LeftButton)
    {
      return;
    }
    const QList<Segment> List = Segments();
    int Index = SegmentAt(List, static_cast<int>(Event->position().x()));
    if (OnClick)
    {
      OnClick();
    }
    if (Index < 0)
    {
      return;
    }
    const Segment & S = List.at(Index);
    if (S.Mask)
    {
      if (OnMaskClick) OnMaskClick();
    }
    else if (!S.Path.isEmpty() && (S.Path != FPath))
    {
      if (OnPathClick) OnPathClick(S.Path);
    }
  }

private:
  QString FPath;
  QString FMask;
  bool FActive = false;
  bool FHover = false;
  int FMouseX = 0;
};

// Status bar panel reacting to a single click
class StatusPanel : public QLabel
{
public:
  using QLabel::QLabel;
  std::function<void()> OnClick;

protected:
  void mousePressEvent(QMouseEvent * Event) override
  {
    if ((Event->button() == Qt::LeftButton) && OnClick)
    {
      OnClick();
    }
    QLabel::mousePressEvent(Event);
  }
};

// WinSCP column indexes of DirViewParams (TUnixDirViewCol, TDirViewCol) to model
// columns. Local "Attr" shows the Unix rights; Owner/Group/Link target are
// NixSCP additions stored after WinSCP's six local columns (WinSCP reads only
// as many widths as it has columns, so the file stays compatible).
const int RemoteColumnMap[] = { FileListModel::ColName, FileListModel::ColSize, FileListModel::ColChanged,
  FileListModel::ColRights, FileListModel::ColOwner, FileListModel::ColGroup, FileListModel::ColExt,
  FileListModel::ColLinkTarget, FileListModel::ColType };
const int LocalColumnMap[] = { FileListModel::ColName, FileListModel::ColSize, FileListModel::ColType,
  FileListModel::ColChanged, FileListModel::ColRights, FileListModel::ColExt,
  FileListModel::ColOwner, FileListModel::ColGroup, FileListModel::ColLinkTarget };
const int LocalWinSCPColumns = 6;

QList<FilePanel *> & AllPanels()
{
  static QList<FilePanel *> Panels;
  return Panels;
}

// ContainsTextSemiCaseSensitive (core/Common.cpp)
bool ContainsTextSemiCaseSensitive(const QString & Text, const QString & SubText)
{
  bool CaseSensitive = (SubText.toLower() != SubText);
  return Text.contains(SubText, CaseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive);
}

QString MinimizeName(const QString & Path, int MaxLength)
{
  if (Path.length() <= MaxLength)
  {
    return Path;
  }
  return QStringLiteral("…") + Path.right(MaxLength - 1);
}

} // namespace

//---------------------------------------------------------------------------
// FilePanel::Private
//---------------------------------------------------------------------------

struct FilePanel::Private : public QObject, public PanelViewHandler
{
  enum LoadKind { lkChange, lkHistory, lkRefresh };
  struct LoadRequest
  {
    LoadKind Kind = lkChange;
    QString FocusName;
    int HistoryIndex = 0;     // lkHistory
    bool Silent = false;      // automatic refresh: no error messages
    QString ClimbFrom;        // silent move to the nearest readable parent
  };

  explicit Private(FilePanel * APanel) : QObject(APanel), Q(APanel) {}

  FilePanel * Q;
  QStackedWidget * Stack = nullptr;
  QWidget * PanelPage = nullptr;
  QWidget * PlaceholderPage = nullptr;
  QVBoxLayout * TopDock = nullptr;
  QHBoxLayout * ButtonRow = nullptr;
  QComboBox * PathCombo = nullptr;
  QWidget * HistoryBand = nullptr;
  QToolButton * BackButton = nullptr;
  QToolButton * ForwardButton = nullptr;
  QMenu * BackMenu = nullptr;
  QMenu * ForwardMenu = nullptr;
  QWidget * NavigationBand = nullptr;
  QToolButton * ParentButton = nullptr;
  QToolButton * RootButton = nullptr;
  QToolButton * HomeButton = nullptr;
  QToolButton * RefreshButton = nullptr;
  PathLabel * Label = nullptr;
  PanelView * View = nullptr;
  PanelDelegate * Delegate = nullptr;
  FileListModel * Model = nullptr;
  QFrame * ReadingBar = nullptr;
  QLabel * ReadingLabel = nullptr;
  QWidget * StatusBar = nullptr;
  StatusPanel * StatusLabel = nullptr;
  StatusPanel * HiddenLabel = nullptr;
  StatusPanel * FilteredLabel = nullptr;
  QTimer * ReadingTimer = nullptr;
  QTimer * RenameTimer = nullptr;
  QTimer * WatchTimer = nullptr;
  QTimer * RemoteRefreshTimer = nullptr;
  QTimer * SaveParamsTimer = nullptr;
  QFileSystemWatcher * Watcher = nullptr;
  QRubberBand * RubberBand = nullptr;

  QString Directory;
  QStringList BackHistory;       // last = most recent
  QStringList ForwardHistory;    // last = next
  quint64 Generation = 0;
  bool Loading = false;
  // The load in progress (a reload of the same directory keeps its purpose)
  QString PendingPath;
  LoadRequest PendingRequest;
  int ReadingCount = 0;
  QElapsedTimer LoadTimer;
  qint64 LastLoadMs = 0;
  bool ShowHidden = false;
  bool PendingAutoRefresh = false;
  QString Note;

  // Marking
  TNortonLikeMode NortonLike = nlKeyboard;
  int Anchor = -1;
  bool AllMarkedImplicitly = false;   // FAnyAndAllSelectedImplicitly
  QSet<QString> SavedNames;
  // Mouse
  QPoint PressPos;
  int PressRow = -1;
  Qt::KeyboardModifiers PressMods;
  bool PressWasMarked = false;
  bool PressWasCurrent = false;
  bool Dragging = false;
  QList<int> DragRows;
  QPoint RubberOrigin;              // content coordinates
  QList<int> RubberBase;
  int RenameRow = -1;
  QElapsedTimer FocusTimer;
  bool FilterReinstalled = false;
  // Incremental search
  bool Searching = false;
  QString SearchText;
  bool SearchHaveNext = false;
  int IncrementalSearching = 0;
  // Preferences (ApplyConfiguration)
  TDoubleClickAction DoubleClickAction = dcaEdit;
  bool CopyOnDoubleClickConfirmation = false;
  TIncrementalSearch PanelSearch = isNameStartOnly;
  bool AutoReadDirectoryAfterOp = true;
  QString FileColorsStr;
  QString AppliedFontName;
  int AppliedFontSize = -1;
  bool FontApplied = false;
  // Columns
  bool HeaderUpdating = false;
  int ColumnWidths[FileListModel::ColCount] = {};
  QList<QPair<int, int>> PendingExtraPositions;
  bool StatusBarVisible = true;
  std::function<QMimeData *(const QStringList &)> RemoteDragFactory;

  //-------------------------------------------------------------------------
  // Helpers

  std::shared_ptr<bool> ProviderAlive;

  // The provider, unless it was destroyed meanwhile (tab closing)
  DirectoryProvider * Live() const
  {
    return ((Q->FProvider != nullptr) && ProviderAlive && *ProviderAlive) ? Q->FProvider : nullptr;
  }

  int RowCount() const { return Model->rowCount(); }
  int CurrentRow() const
  {
    QModelIndex Current = View->currentIndex();
    return Current.isValid() ? Current.row() : -1;
  }

  void SetCurrentRow(int Row, bool Scroll = true)
  {
    if ((Row < 0) || (Row >= RowCount()))
    {
      return;
    }
    QModelIndex Index = Model->index(Row, 0);
    View->selectionModel()->setCurrentIndex(Index, QItemSelectionModel::NoUpdate);
    if (Scroll)
    {
      View->scrollTo(Index);
    }
  }

  QString Combine(const QString & Dir, const QString & Name) const
  {
    if (Q->FRemote)
    {
      return UnixPath::Combine(Dir, Name);
    }
    return QDir::cleanPath(QDir(Dir).filePath(Name));
  }

  QString ParentOf(const QString & Path) const
  {
    // local paths: "/" on Linux, "C:/" and UNC roots on Windows
    return Q->FRemote ? UnixPath::Parent(Path) : LocalPath::Parent(Path);
  }

  bool IsRootPath(const QString & Path) const
  {
    return Q->FRemote ? UnixPath::IsRoot(Path) : LocalPath::IsRoot(Path);
  }

  QString NormalizeRequest(const QString & Path) const
  {
    QString Result = Path.trimmed();
    if (!Q->FRemote)
    {
      Result = ExpandHome(Result);
      if (QDir::isRelativePath(Result) && !Directory.isEmpty())
      {
        Result = QDir(Directory).filePath(Result);
      }
      Result = QDir::cleanPath(Result);
    }
    else if (!Result.startsWith(QLatin1Char('/')) && !Result.startsWith(QLatin1Char('~')) && !Directory.isEmpty())
    {
      Result = UnixPath::Combine(Directory, Result);
    }
    return Result;
  }

  // GetMarkedFile/MarkedCount: marked files, or the focused one (Norton-like modes)
  QList<int> OperationRows() const
  {
    QList<int> Rows = Model->MarkedRows();
    if (Rows.isEmpty() && (NortonLike != nlOff))
    {
      int Current = CurrentRow();
      if ((Current >= 0) && !Model->IsParentRow(Current))
      {
        Rows.append(Current);
      }
    }
    return Rows;
  }

  QString FocusedName() const
  {
    int Current = CurrentRow();
    return (Current >= 0) ? Model->File(Current).FileName : QString();
  }

  bool SignalConnected(const QMetaMethod & Signal) const
  {
    return Q->isSignalConnected(Signal);
  }

  //-------------------------------------------------------------------------
  // Loading

  void SetLoading(bool Value)
  {
    if (Loading == Value)
    {
      return;
    }
    Loading = Value;
    if (Loading)
    {
      LoadTimer.start();
      ReadingCount = 0;
      ReadingTimer->start(ReadingProgressDelay);
    }
    else
    {
      LastLoadMs = LoadTimer.isValid() ? LoadTimer.elapsed() : 0;
      ReadingTimer->stop();
      ReadingBar->hide();
      View->viewport()->unsetCursor();
    }
    emit Q->LoadingChanged(Loading);
  }

  void UpdateReadingBar()
  {
    if (!Loading || ReadingTimer->isActive())
    {
      return;
    }
    // DIRECTORY_READING_PROGRESS
    ReadingLabel->setText((ReadingCount > 0) ?
      QStringLiteral("%1 Reading directory").arg(QLocale().toString(ReadingCount)) :
      QStringLiteral("Reading directory..."));
    if (!ReadingBar->isVisible())
    {
      ReadingBar->show();
      View->viewport()->setCursor(Qt::BusyCursor);
    }
  }

  void Load(const QString & Path, const LoadRequest & Request)
  {
    DirectoryProvider * Provider = Live();
    if ((Provider == nullptr) || Path.isEmpty())
    {
      return;
    }
    // A newer request supersedes any pending one; its callback will be ignored
    LoadRequest Effective = Request;
    if (Loading)
    {
      Provider->CancelLoad();
      // A plain reload of the directory being loaded (e.g. the refresh after
      // an operation right after "create directory and focus it") must not
      // lose what the pending request was for: the file to focus, the history
      if ((Request.Kind == lkRefresh) && Request.FocusName.isEmpty() && (Path == PendingPath))
      {
        Effective = PendingRequest;
        Effective.Silent = Request.Silent && PendingRequest.Silent;
      }
    }
    PendingPath = Path;
    PendingRequest = Effective;
    quint64 Gen = ++Generation;
    SetLoading(true);
    QPointer<FilePanel> Guard(Q);
    auto Progress = [Guard, Gen](int Count)
    {
      if (Guard.isNull() || (Guard->d->Generation != Gen))
      {
        return;
      }
      Guard->d->ReadingCount = Count;
      Guard->d->UpdateReadingBar();
      emit Guard->ReadingProgress(Count);
    };
    Provider->LoadWithProgress(Path, Progress,
      [Guard, Gen, Provider, Request = Effective](bool Ok, const QString & Dir, const RemoteFileList & Files,
        const QString & Error, const QString & More)
      {
        if (Guard.isNull())
        {
          return;
        }
        Private * D = Guard->d;
        if ((Gen != D->Generation) || (Provider != Guard->FProvider))
        {
          return;
        }
        D->LoadDone(Ok, Dir, Files, Error, More, Request);
      });
  }

  void LoadDone(bool Ok, const QString & Dir, const RemoteFileList & Files, const QString & Error,
    const QString & More, LoadRequest Request)
  {
    SetLoading(false);
    if (!Ok)
    {
      PathCombo->setEditText(Directory);
      if (Request.Silent)
      {
        // The watched local directory disappeared: go to the nearest existing
        // parent; when that cannot be read either, continue upwards (never
        // retry the same directory, which would loop forever)
        if (!Q->FRemote && !Directory.isEmpty())
        {
          QString Parent;
          if (!Request.ClimbFrom.isEmpty())
          {
            if (!IsRootPath(Request.ClimbFrom))
            {
              Parent = ParentOf(Request.ClimbFrom);
            }
          }
          else if (!QFileInfo(Directory).isDir())
          {
            Parent = Directory;
            while (!IsRootPath(Parent) && !QFileInfo(Parent).isDir())
            {
              Parent = ParentOf(Parent);
            }
          }
          if (!Parent.isEmpty())
          {
            LoadRequest Up;
            Up.Silent = true;
            Up.ClimbFrom = Parent;
            Load(Parent, Up);
          }
        }
        return;
      }
      emit Q->LoadFailed(Error, More);
      return;
    }

    QString OldDirectory = Directory;
    bool SameDirectory = (Dir == OldDirectory);
    QString FocusName = Request.FocusName;
    QString TopName;
    if (SameDirectory)
    {
      if (FocusName.isEmpty())
      {
        FocusName = FocusedName();
      }
      QModelIndex Top = View->indexAt(QPoint(1, 1));
      if (Top.isValid())
      {
        TopName = Model->File(Top.row()).FileName;
      }
    }
    else if (FocusName.isEmpty() && !OldDirectory.isEmpty() && (ParentOf(OldDirectory) == Dir))
    {
      // Going up (Back to parent etc.): focus the directory we came from
      FocusName = UnixPath::ExtractFileName(OldDirectory);
    }

    switch (Request.Kind)
    {
      case lkChange:
        if (!SameDirectory && !OldDirectory.isEmpty())
        {
          BackHistory.append(OldDirectory);
          ForwardHistory.clear();
        }
        break;
      case lkHistory:
        {
          // TCustomDirView::HistoryGo
          QString Current = OldDirectory;
          if (Request.HistoryIndex < 0)
          {
            for (int Step = 0; (Step < -Request.HistoryIndex) && !BackHistory.isEmpty(); Step++)
            {
              if (!Current.isEmpty()) ForwardHistory.append(Current);
              Current = BackHistory.takeLast();
            }
          }
          else
          {
            for (int Step = 0; (Step < Request.HistoryIndex) && !ForwardHistory.isEmpty(); Step++)
            {
              if (!Current.isEmpty()) BackHistory.append(Current);
              Current = ForwardHistory.takeLast();
            }
          }
        }
        break;
      case lkRefresh:
        break;
    }
    while (BackHistory.size() > MaxHistoryCount)
    {
      BackHistory.removeFirst();
    }

    Directory = Dir;
    if (!SameDirectory)
    {
      ResetIncrementalSearch();
      SavedNames.clear();
      AllMarkedImplicitly = false;
      Note.clear();
    }
    Anchor = -1;
    {
      QSignalBlocker Blocker(View->selectionModel());
      Model->SetFiles(Dir, Files);
    }
    RestoreFocus(FocusName, TopName);
    UpdatePathCombo();
    UpdateButtons();
    UpdatePathLabel();
    UpdateStatus();
    ArmWatcher();
    if (RemoteRefreshTimer->isActive())
    {
      RemoteRefreshTimer->start();   // the interval counts from the last load
    }
    if (!Q->FRemote)
    {
      ApplySwappedPanels(Configuration::Instance().SwappedPanels);
    }
    if (!SameDirectory)
    {
      emit Q->DirectoryChanged(Directory);
    }
    emit Q->SelectionChanged();
  }

  void RestoreFocus(const QString & FocusName, const QString & TopName)
  {
    int Count = RowCount();
    if (!TopName.isEmpty())
    {
      int TopRow = Model->RowOfName(TopName);
      if (TopRow >= 0)
      {
        View->scrollTo(Model->index(TopRow, 0), QAbstractItemView::PositionAtTop);
      }
    }
    int FocusRow = FocusName.isEmpty() ? -1 : Model->RowOfName(FocusName);
    if ((FocusRow < 0) && (Count > 0))
    {
      // FocusSomething
      FocusRow = 0;
    }
    if (FocusRow >= 0)
    {
      IncrementalSearching++;
      SetCurrentRow(FocusRow);
      IncrementalSearching--;
    }
  }

  void UpdatePathCombo()
  {
    QSignalBlocker Blocker(PathCombo);
    if (Q->FRemote)
    {
      // Path components from the root to the current directory
      PathCombo->clear();
      QString Path = QStringLiteral("/");
      QIcon Folder = FileListModel::IconFor([]() { RemoteFile F; F.IsDirectory = true; return F; }());
      PathCombo->addItem(Folder, Path);
      PathCombo->setItemData(0, 0, PathComboDelegate::LevelRole);
      const QStringList Names = Directory.split(QLatin1Char('/'), Qt::SkipEmptyParts);
      for (int Index = 0; Index < Names.size(); Index++)
      {
        Path = UnixPath::Combine(Path, Names.at(Index));
        PathCombo->addItem(Folder, Path);
        PathCombo->setItemData(PathCombo->count() - 1, Index + 1, PathComboDelegate::LevelRole);
      }
      PathCombo->setCurrentIndex(PathCombo->count() - 1);
    }
    else
    {
      // Shown as the user writes them (C:\Users\... on Windows)
      QString Shown = LocalPath::Display(Directory);
      int Existing = PathCombo->findText(Shown, Qt::MatchFixedString);
      if (Existing >= 0)
      {
        PathCombo->removeItem(Existing);
      }
      PathCombo->insertItem(0, Shown);
      while (PathCombo->count() > 30)
      {
        PathCombo->removeItem(PathCombo->count() - 1);
      }
#ifdef Q_OS_WIN
      // The drives, always at hand
      for (const QString & Root : LocalPath::Roots())
      {
        QString Drive = LocalPath::Display(Root);
        if (PathCombo->findText(Drive, Qt::MatchFixedString) < 0)
        {
          PathCombo->addItem(Drive);
        }
      }
#endif
      PathCombo->setCurrentIndex(0);
    }
    PathCombo->setEditText(Q->FRemote ? Directory : LocalPath::Display(Directory));
  }

  void UpdateButtons()
  {
    bool Connected = (Q->FProvider != nullptr) && !Directory.isEmpty();
    BackButton->setEnabled(Q->CanBack());
    ForwardButton->setEnabled(Q->CanForward());
    // EXPLORER_BACK_HINT / EXPLORER_FORWARD_HINT
    BackButton->setToolTip(Q->CanBack() ? QStringLiteral("Back to %1").arg(Q->HistoryPath(-1)) : QStringLiteral("Back"));
    ForwardButton->setToolTip(Q->CanForward() ? QStringLiteral("Forward to %1").arg(Q->HistoryPath(1)) : QStringLiteral("Forward"));
    bool AtRoot = IsRootPath(Directory);
    ParentButton->setEnabled(Connected && !AtRoot);
    RootButton->setEnabled(Connected && !AtRoot);
    HomeButton->setEnabled(Q->FProvider != nullptr);
    RefreshButton->setEnabled(Connected);
  }

  void UpdatePathLabel()
  {
    Label->SetPath(Directory);
    Label->SetMask(Model->FilterMask());
  }

  void UpdateStatus()
  {
    QLocale Locale;
    QString Text;
    if (Searching)
    {
      // FormatIncrementalSearchStatus: INC_SEARCH, INC_SEARCH_TYPE, INC_NEXT_SEARCH
      Text = QStringLiteral("Search: %1").arg(SearchText.isEmpty() ? QStringLiteral("(start typing)") : SearchText);
      if (SearchHaveNext && !SearchText.isEmpty())
      {
        Text += QStringLiteral(" (press Tab for next)");
      }
    }
    else if (!Note.isEmpty())
    {
      Text = Note;
    }
    else if ((Q->FProvider != nullptr) || !Q->FRemote)
    {
      // FILE_INFO_FORMAT
      Text = QStringLiteral("%1 of %2 in %3 of %4").arg(
        FileListModel::FormatBytes(Model->MarkedSize()), FileListModel::FormatBytes(Model->TotalSize()),
        Locale.toString(Model->MarkedCount()), Locale.toString(Model->FileCount()));
    }
    StatusLabel->setText(Translator::TrMessage(Text));
    // FILE_INFO_HIDDEN2 / FILE_INFO_FILTERED2 (UpdateFileStatusExtendedPanels)
    int Hidden = Model->HiddenCount();
    HiddenLabel->setVisible(Hidden > 0);
    HiddenLabel->setText(Translator::TrMessage(QStringLiteral("%1 hidden").arg(Locale.toString(Hidden))));
    int Filtered = Model->FilteredCount();
    FilteredLabel->setVisible(Filtered > 0);
    FilteredLabel->setText(Translator::TrMessage(QStringLiteral("%1 filtered").arg(Locale.toString(Filtered))));
    FilteredLabel->setToolTip(QStringLiteral("Filter: %1").arg(Model->FilterMask()));
  }

  void PostNote(const QString & Text)
  {
    Note = Text;
    UpdateStatus();
    QTimer::singleShot(5000, this, [this, Text]()
    {
      if (Note == Text)
      {
        Note.clear();
        UpdateStatus();
      }
    });
  }

  //-------------------------------------------------------------------------
  // Executing files (TCustomDirView::Execute, TCustomScpExplorerForm::DoDirViewExecFile)

  void ExecuteRow(int Row, bool ForceEnter)
  {
    if ((Row < 0) || (Row >= RowCount()))
    {
      return;
    }
    ResetIncrementalSearch();
    RemoteFile File = Model->File(Row);
    if (File.IsParentDirectory)
    {
      Q->ParentDirectory();
      return;
    }
    QString Path = Combine(Directory, File.FileName);
    if (File.IsDirectory)
    {
      Q->ChangeDirectory(Path);
      return;
    }
    if (ForceEnter)
    {
      return;
    }
    // "Operation on double-click" applies to files only
    if ((DoubleClickAction == dcaCopy) && SignalConnected(QMetaMethod::fromSignal(&FilePanel::CopyRequested)))
    {
      emit Q->CopyRequested(File, Path, !CopyOnDoubleClickConfirmation);
    }
    else if ((DoubleClickAction == dcaEdit) && SignalConnected(QMetaMethod::fromSignal(&FilePanel::EditRequested)))
    {
      emit Q->EditRequested(File, Path);
    }
    else
    {
      emit Q->OpenFileRequested(File, Path);
    }
  }

  //-------------------------------------------------------------------------
  // Marking

  void MarkRange(int From, int To, bool Replace)
  {
    if (Replace)
    {
      Model->MarkAll(false);
    }
    Model->SetMarkedRange(From, To, true);
  }

  void ToggleRow(int Row)
  {
    if ((Row >= 0) && !Model->IsParentRow(Row))
    {
      Model->ToggleMarked(Row);
    }
    AllMarkedImplicitly = false;
  }

  void MarkOnly(int Row)
  {
    // The clicked/focused file becomes the only marked one (implicit selection)
    QList<int> Rows;
    if ((Row >= 0) && !Model->IsParentRow(Row))
    {
      Rows.append(Row);
    }
    if (!((Model->MarkedCount() == Rows.size()) && (Rows.isEmpty() || Model->IsMarked(Row))))
    {
      Model->SetMarkedRows(Rows, true);
    }
    AllMarkedImplicitly = !Rows.isEmpty();
  }

  //-------------------------------------------------------------------------
  // Incremental search (TCustomScpExplorerForm::IncrementalSearch)

  void ResetIncrementalSearch()
  {
    if (Searching)
    {
      Searching = false;
      SearchText.clear();
      SearchHaveNext = false;
      UpdateStatus();
    }
  }

  bool SearchMatches(int Row, const QString & Text) const
  {
    const RemoteFile & File = Model->File(Row);
    switch (PanelSearch)
    {
      case isNameStartOnly:
        return ContainsTextSemiCaseSensitive(File.FileName.left(Text.length()), Text);
      case isName:
        return ContainsTextSemiCaseSensitive(File.FileName, Text);
      case isAll:
        for (int Column = 0; Column < FileListModel::ColCount; Column++)
        {
          if (!View->header()->isSectionHidden(Column) &&
              ContainsTextSemiCaseSensitive(Model->index(Row, Column).data().toString(), Text))
          {
            return true;
          }
        }
        return false;
      case isOff:
        break;
    }
    return false;
  }

  int SearchFile(const QString & Text, bool SkipCurrent, bool Reverse) const
  {
    int Count = RowCount();
    if (Count == 0)
    {
      return -1;
    }
    int Current = std::max(CurrentRow(), 0);
    auto Next = [Count, Reverse](int Row) { return Reverse ? ((Row + Count - 1) % Count) : ((Row + 1) % Count); };
    int Row = SkipCurrent ? Next(Current) : Current;
    while (true)
    {
      if (SearchMatches(Row, Text))
      {
        return Row;
      }
      Row = Next(Row);
      if (Row == Current)
      {
        return SkipCurrent && SearchMatches(Current, Text) ? Current : -1;
      }
    }
  }

  void IncrementalSearch(const QString & Text, bool SkipCurrent, bool Reverse)
  {
    int Row = SearchFile(Text, SkipCurrent, Reverse);
    if (Row < 0)
    {
      QApplication::beep();
      return;
    }
    IncrementalSearching++;
    SetCurrentRow(Row);
    if (NortonLike == nlOff)
    {
      MarkOnly(Row);
    }
    IncrementalSearching--;
    Searching = true;
    SearchText = Text;
    int NextRow = SearchFile(Text, true, Reverse);
    SearchHaveNext = (NextRow >= 0) && (NextRow != Row);
    UpdateStatus();
  }

  //-------------------------------------------------------------------------
  // PanelViewHandler: keyboard

  void KeyboardContextMenu()
  {
    int Row = CurrentRow();
    QPoint Pos;
    if (Row >= 0)
    {
      QRect Rect = View->visualRect(Model->index(Row, FileListModel::ColName));
      Pos = Rect.bottomLeft() + QPoint(std::min(Rect.width() / 2, 40), 0);
      if (!View->viewport()->rect().contains(Pos))
      {
        Pos = View->viewport()->rect().center();
      }
    }
    else
    {
      Pos = QPoint(4, 4);
    }
    emit Q->ContextMenuRequested(View->viewport()->mapToGlobal(Pos), (Row >= 0) && !Model->IsParentRow(Row));
  }

  bool NavigationKey(int Key, Qt::KeyboardModifiers Mods)
  {
    if ((Key != Qt::Key_Up) && (Key != Qt::Key_Down) && (Key != Qt::Key_PageUp) && (Key != Qt::Key_PageDown) &&
        (Key != Qt::Key_Home) && (Key != Qt::Key_End))
    {
      return false;
    }
    if (RowCount() == 0)
    {
      return true;
    }
    int OldRow = CurrentRow();
    QModelIndex NewIndex = View->MoveCursorPublic(Key);
    int NewRow = NewIndex.isValid() ? NewIndex.row() : ((OldRow >= 0) ? OldRow : 0);
    if (OldRow < 0)
    {
      OldRow = NewRow;
    }
    bool Shift = Mods.testFlag(Qt::ShiftModifier);
    bool Ctrl = Mods.testFlag(Qt::ControlModifier);
    if (Shift)
    {
      if (Anchor < 0)
      {
        Anchor = OldRow;
      }
      // nlOn and explicit marks: Shift only adds (FDontUnSelectItem)
      bool Replace = (NortonLike == nlOff) ? !Ctrl : ((NortonLike == nlKeyboard) && AllMarkedImplicitly);
      MarkRange(Anchor, NewRow, Replace);
      AllMarkedImplicitly = false;
    }
    else if (NortonLike == nlOff)
    {
      if (!Ctrl)
      {
        MarkOnly(NewRow);
      }
      Anchor = NewRow;
    }
    else
    {
      // Norton-like: the cursor moves, marks stay; only an implicit (mouse)
      // selection is dropped with "Mouse only" explorer-style selection
      if ((NortonLike == nlKeyboard) && AllMarkedImplicitly && !Ctrl && (Model->MarkedCount() > 0))
      {
        Model->MarkAll(false);
      }
      AllMarkedImplicitly = false;
      Anchor = NewRow;
    }
    SetCurrentRow(NewRow);
    return true;
  }

  bool ViewKeyPress(QKeyEvent * Event) override
  {
    if (View->IsEditing())
    {
      return false;
    }
    RenameTimer->stop();
    Qt::KeyboardModifiers Mods = Event->modifiers() & ~(Qt::KeypadModifier | Qt::GroupSwitchModifier);
    int Key = Event->key();

    if (Key == Qt::Key_Escape)
    {
      if (Searching)
      {
        ResetIncrementalSearch();
        return true;
      }
      if (Loading)
      {
        Q->CancelLoading();
        return true;
      }
      return false;
    }
    if (Searching && (Key == Qt::Key_Backspace) && (Mods == Qt::NoModifier))
    {
      if (SearchText.length() <= 1)
      {
        ResetIncrementalSearch();
      }
      else
      {
        IncrementalSearch(SearchText.left(SearchText.length() - 1), false, false);
      }
      return true;
    }
    if (Searching && ((Key == Qt::Key_Tab) || (Key == Qt::Key_Backtab)) && !SearchText.isEmpty())
    {
      IncrementalSearch(SearchText, true, (Key == Qt::Key_Backtab) || Mods.testFlag(Qt::ShiftModifier));
      return true;
    }

    switch (Key)
    {
      case Qt::Key_Return:
      case Qt::Key_Enter:
        if (Mods == Qt::NoModifier)
        {
          ExecuteRow(CurrentRow(), false);
          return true;
        }
        if (Mods == Qt::AltModifier)
        {
          emit Q->PropertiesRequested();
          return true;
        }
        return false;
      case Qt::Key_Backspace:
        if (Mods == Qt::NoModifier)
        {
          Q->ParentDirectory();
          return true;
        }
        return false;
      case Qt::Key_Backslash:
        if (Mods == Qt::ControlModifier)
        {
          Q->RootDirectory();
          return true;
        }
        break;
      case Qt::Key_Left:
      case Qt::Key_Right:
        if (Mods == Qt::AltModifier)
        {
          if (Key == Qt::Key_Left) Q->Back(); else Q->Forward();
          return true;
        }
        // Norton-like: Left/Right go to the first/last file when there is
        // no horizontal scroll bar
        if ((NortonLike != nlOff) && !(Mods & (Qt::ControlModifier | Qt::AltModifier)) &&
            !View->horizontalScrollBar()->isVisible())
        {
          ResetIncrementalSearch();
          return NavigationKey((Key == Qt::Key_Left) ? Qt::Key_Home : Qt::Key_End, Mods);
        }
        return false;
      case Qt::Key_Up:
        if (Mods == Qt::AltModifier)
        {
          Q->ParentDirectory();
          return true;
        }
        if (Mods & Qt::AltModifier)
        {
          return false;
        }
        ResetIncrementalSearch();
        return NavigationKey(Key, Mods);
      case Qt::Key_PageUp:
        if (Mods == Qt::ControlModifier)
        {
          Q->ParentDirectory();
          return true;
        }
        if (Mods & Qt::AltModifier)
        {
          return false;
        }
        ResetIncrementalSearch();
        return NavigationKey(Key, Mods);
      case Qt::Key_PageDown:
        if (Mods == Qt::ControlModifier)
        {
          ExecuteRow(CurrentRow(), true);
          return true;
        }
        if (Mods & Qt::AltModifier)
        {
          return false;
        }
        ResetIncrementalSearch();
        return NavigationKey(Key, Mods);
      case Qt::Key_Down:
      case Qt::Key_Home:
      case Qt::Key_End:
        if (Mods & Qt::AltModifier)
        {
          return false;
        }
        ResetIncrementalSearch();
        return NavigationKey(Key, Mods);
      case Qt::Key_R:
        if (Mods == Qt::ControlModifier)
        {
          Q->Refresh();
          return true;
        }
        break;
      case Qt::Key_Insert:
        if (Mods == Qt::NoModifier)
        {
          ResetIncrementalSearch();
          Q->SelectCurrentItem();
          return true;
        }
        return false;
      case Qt::Key_Space:
        if (Mods == Qt::ControlModifier)
        {
          ToggleRow(CurrentRow());
          return true;
        }
        if (Mods == Qt::NoModifier)
        {
          // While searching, the space is a part of the searched text
          if (Searching && (PanelSearch != isOff))
          {
            IncrementalSearch(SearchText + QLatin1Char(' '), false, false);
          }
          else if (NortonLike != nlOff)
          {
            ToggleRow(CurrentRow());
          }
          else
          {
            int Row = CurrentRow();
            if ((Row >= 0) && !Model->IsParentRow(Row))
            {
              Model->SetMarked(Row, true);
            }
          }
          return true;
        }
        return false;
      case Qt::Key_Menu:
        KeyboardContextMenu();
        return true;
      case Qt::Key_F10:
        if (Mods == Qt::ShiftModifier)
        {
          KeyboardContextMenu();
          return true;
        }
        return false;
      default:
        break;
    }

    // Num+, Num-, Num* are the Select/Unselect/Invert commands, never searched for
    // (TCustomNortonLikeListView FNextCharToIgnore)
    if (Event->modifiers().testFlag(Qt::KeypadModifier) &&
        ((Key == Qt::Key_Plus) || (Key == Qt::Key_Minus) || (Key == Qt::Key_Asterisk)))
    {
      return true;
    }
    // Incremental search (DirViewKeyPress)
    QString Text = Event->text();
    if (!Text.isEmpty() && (Text.at(0) > QLatin1Char(' ')) && Text.at(0).isPrint() &&
        !(Mods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)))
    {
      if (PanelSearch != isOff)
      {
        IncrementalSearch(SearchText + Text, false, false);
      }
      return true;
    }
    return false;
  }

  //-------------------------------------------------------------------------
  // PanelViewHandler: mouse (TCustomNortonLikeListView WMLButtonDown/Up, WMRButtonDown)

  int RowAtY(int Y) const
  {
    QModelIndex Index = View->indexAt(QPoint(1, Y));
    if (Index.isValid())
    {
      return Index.row();
    }
    int Count = RowCount();
    if (Count == 0)
    {
      return -1;
    }
    QRect Last = View->visualRect(Model->index(Count - 1, 0));
    if (Y > Last.bottom())
    {
      return Count - 1;
    }
    return (Y < 0) ? 0 : -1;
  }

  void UpdateRubberBand(const QPoint & Pos)
  {
    QPoint Origin = RubberOrigin - View->ContentOffset();
    QRect Rect = QRect(Origin, Pos).normalized();
    RubberBand->setGeometry(Rect.intersected(View->viewport()->rect().adjusted(-1, -1, 1, 1)));
    QList<int> Rows = RubberBase;
    if (RowCount() > 0)
    {
      QRect Last = View->visualRect(Model->index(RowCount() - 1, 0));
      if (Rect.top() <= Last.bottom())
      {
        int First = std::max(RowAtY(Rect.top()), 0);
        int LastRow = RowAtY(Rect.bottom());
        for (int Row = First; (LastRow >= 0) && (Row <= LastRow); Row++)
        {
          Rows.append(Row);
        }
      }
    }
    Model->SetMarkedRows(Rows, true);
  }

  bool ViewMousePress(QMouseEvent * Event) override
  {
    RenameTimer->stop();
    ResetIncrementalSearch();
    QPoint Pos = Event->position().toPoint();
    QModelIndex Index = View->indexAt(Pos);
    int Row = Index.isValid() ? Index.row() : -1;
    Qt::KeyboardModifiers Mods = Event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier);
    if (Event->button() == Qt::BackButton)
    {
      Q->Back();
      return true;
    }
    if (Event->button() == Qt::ForwardButton)
    {
      Q->Forward();
      return true;
    }
    bool HadFocus = View->hasFocus();
    View->setFocus(Qt::MouseFocusReason);
    if (Event->button() == Qt::LeftButton)
    {
      PressPos = Pos;
      PressRow = Row;
      PressMods = Mods;
      Dragging = false;
      PressWasMarked = (Row >= 0) && Model->IsMarked(Row);
      PressWasCurrent = (Row >= 0) && (Row == CurrentRow()) && HadFocus && FocusTimer.isValid() &&
        (FocusTimer.elapsed() > 750);
      if (Row < 0)
      {
        // Background: Explorer-style selection clears the marks, rubber band selects
        if ((NortonLike != nlOn) && (Mods == Qt::NoModifier))
        {
          Model->MarkAll(false);
          AllMarkedImplicitly = false;
        }
        if ((NortonLike != nlOn) || Mods.testFlag(Qt::ControlModifier))
        {
          RubberOrigin = Pos + View->ContentOffset();
          RubberBase = Mods.testFlag(Qt::ControlModifier) || (NortonLike == nlOn) ? Model->MarkedRows() : QList<int>();
          if (RubberBand == nullptr)
          {
            RubberBand = new QRubberBand(QRubberBand::Rectangle, View->viewport());
          }
          RubberBand->setGeometry(QRect(Pos, QSize()));
          RubberBand->show();
        }
        return true;
      }
      if (Mods == Qt::ControlModifier)
      {
        ToggleRow(Row);
        Anchor = Row;
      }
      else if (Mods.testFlag(Qt::ShiftModifier))
      {
        if (Anchor < 0)
        {
          Anchor = (CurrentRow() >= 0) ? CurrentRow() : Row;
        }
        bool Replace = (NortonLike != nlOn) && !Mods.testFlag(Qt::ControlModifier);
        MarkRange(Anchor, Row, Replace);
        AllMarkedImplicitly = false;
      }
      else
      {
        if ((NortonLike != nlOn) && !Model->IsMarked(Row))
        {
          // Clicking outside of the selection clears it right away
          MarkOnly(Row);
        }
        Anchor = Row;
      }
      IncrementalSearching++;
      SetCurrentRow(Row, false);
      IncrementalSearching--;
      return true;
    }
    if (Event->button() == Qt::RightButton)
    {
      if (Row >= 0)
      {
        if ((NortonLike != nlOn) && !Model->IsMarked(Row) && (Mods == Qt::NoModifier))
        {
          MarkOnly(Row);
        }
        SetCurrentRow(Row, false);
      }
      return true;
    }
    return false;
  }

  bool ViewMouseMove(QMouseEvent * Event) override
  {
    QPoint Pos = Event->position().toPoint();
    if ((RubberBand != nullptr) && RubberBand->isVisible())
    {
      UpdateRubberBand(Pos);
      return true;
    }
    if (!(Event->buttons() & Qt::LeftButton))
    {
      return false;
    }
    if ((PressRow >= 0) && !Dragging && ((Pos - PressPos).manhattanLength() >= QApplication::startDragDistance()) &&
        !Model->IsParentRow(PressRow))
    {
      Dragging = true;
      // The dragged files: all marked when dragging a marked file, the file itself otherwise
      DragRows = Model->IsMarked(PressRow) ? Model->MarkedRows() : QList<int>{PressRow};
      View->StartDragPublic();
      return true;
    }
    return true;
  }

  bool ViewMouseRelease(QMouseEvent * Event) override
  {
    if ((RubberBand != nullptr) && RubberBand->isVisible())
    {
      RubberBand->hide();
      AllMarkedImplicitly = false;
      return true;
    }
    if ((Event->button() == Qt::LeftButton) && (PressRow >= 0) && !Dragging)
    {
      bool Plain = (PressMods == Qt::NoModifier);
      if (Plain && (NortonLike != nlOn))
      {
        // WMLButtonUp: a click selects the clicked file only
        MarkOnly(PressRow);
      }
      // Click-to-rename: a slow second click on the name of the focused file
      QModelIndex NameIndex = Model->index(PressRow, FileListModel::ColName);
      bool OnName = View->visualRect(NameIndex).contains(PressPos);
      bool Selected = (NortonLike == nlOn) ? PressWasMarked : (PressWasMarked && (Model->MarkedCount() == 1));
      if (Plain && PressWasCurrent && Selected && OnName && (Model->flags(NameIndex) & Qt::ItemIsEditable))
      {
        RenameRow = PressRow;
        RenameTimer->start(QApplication::doubleClickInterval() + 50);
      }
    }
    PressRow = -1;
    Dragging = false;
    return true;
  }

  bool ViewMouseDoubleClick(QMouseEvent * Event) override
  {
    RenameTimer->stop();
    if (Event->button() != Qt::LeftButton)
    {
      return true;
    }
    QModelIndex Index = View->indexAt(Event->position().toPoint());
    if (Index.isValid())
    {
      if (Event->modifiers().testFlag(Qt::AltModifier))
      {
        emit Q->PropertiesRequested();
      }
      else
      {
        ExecuteRow(Index.row(), false);
      }
    }
    return true;
  }

  //-------------------------------------------------------------------------
  // PanelViewHandler: drag & drop

  QMimeData * ViewCreateDragData() override
  {
    QStringList Paths;
    for (int Row : DragRows)
    {
      if (!Model->IsParentRow(Row))
      {
        Paths.append(Combine(Directory, Model->File(Row).FileName));
      }
    }
    if (Paths.isEmpty())
    {
      return nullptr;
    }
    QMimeData * Data = nullptr;
    if (Q->FRemote && RemoteDragFactory)
    {
      Data = RemoteDragFactory(Paths);
    }
    if (Data == nullptr)
    {
      Data = new QMimeData();
    }
    QJsonObject Obj;
    Obj.insert(QStringLiteral("remote"), Q->FRemote);
    Obj.insert(QStringLiteral("paths"), QJsonArray::fromStringList(Paths));
    Data->setData(FilesMimeType, QJsonDocument(Obj).toJson(QJsonDocument::Compact));
    if (!Q->FRemote)
    {
      QList<QUrl> Urls;
      for (const QString & Path : Paths)
      {
        Urls.append(QUrl::fromLocalFile(Path));
      }
      Data->setUrls(Urls);
    }
    else
    {
      emit Q->DragOutRequested(Paths, Data);
    }
    return Data;
  }

  void ViewDragFinished() override
  {
    DragRows.clear();
    PressRow = -1;
    Dragging = false;
  }

  bool ViewIsDropTargetRow(int Row) override
  {
    return Model->File(Row).IsDirectory;
  }

  bool ViewIsRowDragged(int Row) override
  {
    return DragRows.contains(Row);
  }

  void ViewDrop(const DropData & Data, int Row, Qt::DropAction Action) override
  {
    QString Target = Directory;
    if (Row >= 0)
    {
      const RemoteFile & File = Model->File(Row);
      Target = File.IsParentDirectory ? ParentOf(Directory) : Combine(Directory, File.FileName);
    }
    emit Q->DropRequested(Data.Paths, Data.FromRemote, Target, Action);
  }

  bool ViewIsMarked(int Row) override
  {
    return Model->IsMarked(Row);
  }

  bool ViewFullRowSelect() override
  {
    return Delegate->FullRowSelect;
  }

  //-------------------------------------------------------------------------
  // Events of the view, the path combo and the panel itself

  bool eventFilter(QObject * Watched, QEvent * Event) override
  {
    if (Watched == View)
    {
      switch (Event->type())
      {
        case QEvent::FocusIn:
          FocusTimer.start();
          if (!FilterReinstalled)
          {
            // Be the first filter of the view (Tab while searching)
            FilterReinstalled = true;
            QTimer::singleShot(0, this, [this]() { View->installEventFilter(this); });
          }
          View->viewport()->update();
          emit Q->Activated();
          break;
        case QEvent::FocusOut:
          ResetIncrementalSearch();
          RenameTimer->stop();
          View->viewport()->update();
          break;
        case QEvent::KeyPress:
          {
            QKeyEvent * Key = static_cast<QKeyEvent *>(Event);
            if (((Key->key() == Qt::Key_Tab) || (Key->key() == Qt::Key_Backtab)) && Searching &&
                !SearchText.isEmpty() && !View->IsEditing())
            {
              ViewKeyPress(Key);
              return true;
            }
          }
          break;
        default:
          break;
      }
    }
    else if (Watched == PathCombo)
    {
      if (Event->type() == QEvent::FocusIn)
      {
        emit Q->Activated();
      }
    }
    else if (Watched == Q)
    {
      if ((Event->type() == QEvent::Show) && PendingAutoRefresh)
      {
        PendingAutoRefresh = false;
        WatchTimer->start(0);
      }
    }
    return QObject::eventFilter(Watched, Event);
  }

  //-------------------------------------------------------------------------
  // Automatic refresh: local changes (TDiscMonitor -> QFileSystemWatcher/inotify),
  // remote interval (RefreshRemotePanel)

  void ArmWatcher()
  {
    bool Watch = !Q->FRemote && (dynamic_cast<LocalDirectoryProvider *>(Live()) != nullptr) && !Directory.isEmpty();
    if (!Watch)
    {
      if ((Watcher != nullptr) && !Watcher->directories().isEmpty())
      {
        Watcher->removePaths(Watcher->directories());
      }
      return;
    }
    if (Watcher == nullptr)
    {
      Watcher = new QFileSystemWatcher(this);
      connect(Watcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString & Path)
      {
        // the parent (macOS): only when the directory itself has gone
        bool Vanished = (Path != Directory) && !QFileInfo(Directory).isDir();
        if (((Path == Directory) || Vanished) && !WatchTimer->isActive())
        {
          // Debounced; a big directory is not re-read more often than it takes to read it
          WatchTimer->start(std::max<qint64>(300, 2 * LastLoadMs));
        }
      });
    }
    QStringList Wanted{Directory};
#ifdef Q_OS_MACOS
    // FSEvents/kqueue do not report the removal of the watched directory
    // itself: its parent shows it
    if (!IsRootPath(Directory))
    {
      Wanted.append(ParentOf(Directory));
    }
#endif
    QStringList Watched = Watcher->directories();
    if (QSet<QString>(Watched.begin(), Watched.end()) != QSet<QString>(Wanted.begin(), Wanted.end()))
    {
      if (!Watched.isEmpty())
      {
        Watcher->removePaths(Watched);
      }
      Watcher->addPaths(Wanted);
    }
  }

  void AutoRefresh()
  {
    DirectoryProvider * Provider = Live();
    if ((Provider == nullptr) || Directory.isEmpty())
    {
      return;
    }
    if (!Q->isVisible())
    {
      PendingAutoRefresh = true;
      return;
    }
    if (Loading || View->IsEditing() || (QApplication::mouseButtons() != Qt::NoButton) || Provider->IsBusy())
    {
      WatchTimer->start(300);
      return;
    }
    LoadRequest Request;
    Request.Kind = lkRefresh;
    Request.Silent = true;
    Load(Directory, Request);
  }

  void RemoteRefreshTimeout()
  {
    DirectoryProvider * Provider = Live();
    if ((Provider == nullptr) || Directory.isEmpty() || !Q->isVisible() || Loading ||
        View->IsEditing() || Provider->IsBusy() || (QApplication::mouseButtons() != Qt::NoButton))
    {
      return;   // tried again after the next interval
    }
    LoadRequest Request;
    Request.Kind = lkRefresh;
    Request.Silent = true;
    Load(Directory, Request);
  }

  //-------------------------------------------------------------------------
  // Preferences

  void ApplySwappedPanels(bool Swapped)
  {
    // ScpCommander SwappedPanels: local panel on the right
    QSplitter * Splitter = qobject_cast<QSplitter *>(Q->parentWidget());
    if ((Splitter == nullptr) || (Splitter->count() != 2) || Q->FRemote)
    {
      return;
    }
    FilePanel * First = qobject_cast<FilePanel *>(Splitter->widget(0));
    FilePanel * Second = qobject_cast<FilePanel *>(Splitter->widget(1));
    if ((First == nullptr) || (Second == nullptr) || (First->IsRemote() == Second->IsRemote()))
    {
      return;
    }
    int Wanted = Swapped ? 1 : 0;
    if (Splitter->indexOf(Q) != Wanted)
    {
      QList<int> Sizes = Splitter->sizes();
      Splitter->insertWidget(Wanted, Q);
      std::reverse(Sizes.begin(), Sizes.end());
      Splitter->setSizes(Sizes);
    }
  }

  void ApplyConfiguration()
  {
    Configuration & Config = Configuration::Instance();
    NortonLike = Config.NortonLikeMode;
    Model->SetFormatSizeBytes(Config.FormatSizeBytes);
    Model->SetNaturalOrderNumericalSorting(Config.NaturalOrderNumericalSorting);
    Model->SetAlwaysSortDirectoriesByName(Config.AlwaysSortDirectoriesByName);
    DoubleClickAction = Config.DoubleClickAction;
    CopyOnDoubleClickConfirmation = Config.CopyOnDoubleClickConfirmation;
    PanelSearch = Config.PanelSearch;
    if (PanelSearch == isOff)
    {
      ResetIncrementalSearch();
    }
    AutoReadDirectoryAfterOp = Config.AutoReadDirectoryAfterOp;
    if ((Delegate->FullRowSelect != Config.FullRowSelect) || (Delegate->RenameWholeName != Config.RenameWholeName))
    {
      Delegate->FullRowSelect = Config.FullRowSelect;
      Delegate->RenameWholeName = Config.RenameWholeName;
      View->viewport()->update();
    }
    // Panel font (PanelFontName/PanelFontSize; empty = default font)
    if (!FontApplied || (AppliedFontName != Config.PanelFontName) || (AppliedFontSize != Config.PanelFontSize))
    {
      FontApplied = true;
      AppliedFontName = Config.PanelFontName;
      AppliedFontSize = Config.PanelFontSize;
      // A default QFont resolves nothing, so the view inherits the font again
      QFont Font;
      if (!AppliedFontName.isEmpty())
      {
        Font.setFamily(AppliedFontName);
      }
      if (AppliedFontSize > 0)
      {
        Font.setPointSize(AppliedFontSize);
      }
      View->setFont(Font);
    }
    // FileColors and DimmHiddenFiles are kept raw in Configuration\Interface (WinSCP keys)
    {
      IniStorage Storage(&Config.Ini());
      QString Colors;
      bool Dim = true;
      if (Storage.OpenSubKey(QStringLiteral("Configuration"), false) &&
          Storage.OpenSubKey(QStringLiteral("Interface"), false))
      {
        Colors = Storage.ReadString(QStringLiteral("FileColors"), QString());
        Dim = Storage.ReadBool(QStringLiteral("DimmHiddenFiles"), true);
      }
      if (Colors != FileColorsStr)
      {
        FileColorsStr = Colors;
        Model->SetFileColors(FileColorRule::LoadList(Colors));
      }
      Model->SetDimHiddenFiles(Dim);
    }
    if (Q->FRemote)
    {
      Model->SetHideInaccessibleDirectories(!Config.ShowInaccesibleDirectories,
        (Live() != nullptr) ? Live()->UserName() : QString());
      if (Config.RefreshRemotePanel && (Config.RefreshRemotePanelInterval > 0))
      {
        int Interval = std::max(1, Config.RefreshRemotePanelInterval) * 1000;
        if (!RemoteRefreshTimer->isActive() || (RemoteRefreshTimer->interval() != Interval))
        {
          RemoteRefreshTimer->start(Interval);
        }
      }
      else
      {
        RemoteRefreshTimer->stop();
      }
    }
    else
    {
      ApplySwappedPanels(Config.SwappedPanels);
    }
  }

  //-------------------------------------------------------------------------
  // Columns (DirViewParams)

  const int * ColumnMap() const
  {
    return Q->FRemote ? RemoteColumnMap : LocalColumnMap;
  }

  int WinSCPColumns() const
  {
    return Q->FRemote ? 9 : LocalWinSCPColumns;
  }

  int WinSCPIndexOf(int Column) const
  {
    const int * Map = ColumnMap();
    for (int Index = 0; Index < FileListModel::ColCount; Index++)
    {
      if (Map[Index] == Column)
      {
        return Index;
      }
    }
    return -1;
  }

  QString ParamsKey() const
  {
    return Q->FRemote ? QStringLiteral("RemotePanel") : QStringLiteral("LocalPanel");
  }

  bool OpenPanelKey(IniStorage & Storage, bool CanCreate) const
  {
    // Configuration\Interface\Commander\LocalPanel|RemotePanel
    return Storage.OpenSubKey(QStringLiteral("Configuration"), CanCreate) &&
      Storage.OpenSubKey(QStringLiteral("Interface"), CanCreate) &&
      Storage.OpenSubKey(QStringLiteral("Commander"), CanCreate) &&
      Storage.OpenSubKey(ParamsKey(), CanCreate);
  }

  void LoadPanelState()
  {
    IniStorage Storage(&Configuration::Instance().Ini());
    QString Params;
    if (OpenPanelKey(Storage, false))
    {
      Params = Storage.ReadString(QStringLiteral("DirViewParams"), QString());
      StatusBarVisible = Storage.ReadBool(QStringLiteral("StatusBar"), true);
    }
    Q->SetDirViewParams(DefaultDirViewParams(Q->FRemote));
    if (!Params.isEmpty())
    {
      Q->SetDirViewParams(Params);
    }
    StatusBar->setVisible(StatusBarVisible);
  }

  void SavePanelState()
  {
    IniStorage Storage(&Configuration::Instance().Ini());
    if (OpenPanelKey(Storage, true))
    {
      Storage.WriteString(QStringLiteral("DirViewParams"), Q->DirViewParams());
      Storage.WriteBool(QStringLiteral("StatusBar"), StatusBarVisible);
    }
  }

  void ScheduleSaveParams()
  {
    if (!HeaderUpdating)
    {
      SaveParamsTimer->start(300);
    }
  }

  void ShowHeaderMenu(const QPoint & Pos)
  {
    // LocalDirViewColumnPopup / RemoteDirViewColumnPopup (forms/NonVisual.dfm)
    QHeaderView * Header = View->header();
    int Column = Header->logicalIndexAt(Pos);
    QMenu Menu;
    if (Column >= 0)
    {
      QAction * Ascending = Menu.addAction(QStringLiteral("Sort &Ascending"));
      Ascending->setCheckable(true);
      Ascending->setChecked((Model->SortColumn() == Column) && Model->SortAscending());
      connect(Ascending, &QAction::triggered, this, [this, Column]() { Q->SetSort(Column, true); });
      QAction * Descending = Menu.addAction(QStringLiteral("Sort &Descending"));
      Descending->setCheckable(true);
      Descending->setChecked((Model->SortColumn() == Column) && !Model->SortAscending());
      connect(Descending, &QAction::triggered, this, [this, Column]() { Q->SetSort(Column, false); });
      if (Column == FileListModel::ColName)
      {
        QAction * ByExt = Menu.addAction(QStringLiteral("By &Extension"));
        ByExt->setCheckable(true);
        ByExt->setChecked(Model->SortColumn() == FileListModel::ColExt);
        connect(ByExt, &QAction::triggered, this, [this]() { Q->SortBy(FileListModel::ColExt); });
      }
      QAction * Hide = Menu.addAction(QStringLiteral("&Hide Column"));
      Hide->setEnabled(Column != FileListModel::ColName);
      connect(Hide, &QAction::triggered, this, [this, Column]() { Q->SetColumnVisible(Column, false); });
      Menu.addSeparator();
    }
    QMenu * Sizes = Menu.addMenu(QStringLiteral("Show File Si&zes In"));
    const struct { TFormatBytesStyle Style; const char * Caption; } SizeStyles[] =
      { { fbNone, "&Bytes" }, { fbKilobytes, "&Kilobytes" }, { fbShort, "&Short format" } };
    for (const auto & Item : SizeStyles)
    {
      QAction * Action = Sizes->addAction(QString::fromLatin1(Item.Caption));
      Action->setCheckable(true);
      Action->setChecked(Configuration::Instance().FormatSizeBytes == Item.Style);
      TFormatBytesStyle Style = Item.Style;
      connect(Action, &QAction::triggered, this, [Style]()
      {
        Configuration::Instance().FormatSizeBytes = Style;
        for (FilePanel * Panel : AllPanels())
        {
          Panel->ApplyConfiguration();
        }
      });
    }
    Menu.addSeparator();
    QMenu * Columns = Menu.addMenu(QStringLiteral("&Columns"));
    for (int Index = 0; Index < FileListModel::ColCount; Index++)
    {
      int ModelColumn = ColumnMap()[Index];
      QAction * Action = Columns->addAction(ColumnMenuCaption(ModelColumn));
      Action->setCheckable(true);
      Action->setChecked(!Header->isSectionHidden(ModelColumn));
      Action->setEnabled(ModelColumn != FileListModel::ColName);
      connect(Action, &QAction::toggled, this, [this, ModelColumn](bool Checked)
      {
        Q->SetColumnVisible(ModelColumn, Checked);
      });
    }
    Columns->addSeparator();
    QAction * AutoSize = Columns->addAction(QStringLiteral("Size &Automatically"));
    AutoSize->setShortcut(QKeySequence(QKeyCombination(Qt::ControlModifier | Qt::KeypadModifier, Qt::Key_Plus)));
    connect(AutoSize, &QAction::triggered, this, [this]() { Q->AutoSizeColumns(); });
    QAction * Reset = Columns->addAction(QStringLiteral("&Reset Layout"));
    connect(Reset, &QAction::triggered, this, [this]() { Q->ResetColumnLayout(); });
    Menu.exec(Header->viewport()->mapToGlobal(Pos));
  }

  QString ColumnMenuCaption(int Column) const
  {
    // ShowHide*ColumnAction captions
    switch (Column)
    {
      case FileListModel::ColName: return QStringLiteral("&Name");
      case FileListModel::ColExt: return QStringLiteral("&Extension");
      case FileListModel::ColSize: return QStringLiteral("&Size");
      case FileListModel::ColType: return QStringLiteral("&Type");
      case FileListModel::ColChanged: return QStringLiteral("&Date modified");
      case FileListModel::ColRights: return QStringLiteral("&Permissions");
      case FileListModel::ColOwner: return QStringLiteral("&Owner");
      case FileListModel::ColGroup: return QStringLiteral("&Group");
      case FileListModel::ColLinkTarget: return QStringLiteral("&Link Target");
    }
    return QString();
  }

  QToolButton * CreateButton(QWidget * Parent, const QString & ThemeIcon, QStyle::StandardPixmap Fallback,
    const QString & Hint, void (FilePanel::*Method)())
  {
    QToolButton * Button = new QToolButton(Parent);
    Button->setAutoRaise(true);
    Button->setIcon(QIcon::fromTheme(ThemeIcon, Q->style()->standardIcon(Fallback)));
    Button->setToolTip(Hint);
    Button->setFocusPolicy(Qt::NoFocus);
    connect(Button, &QToolButton::clicked, this, [this, Method]()
    {
      (Q->*Method)();
      View->setFocus();
    });
    return Button;
  }

  void PopulateHistoryMenu(QMenu * Menu, bool Back)
  {
    // TCustomScpExplorerForm::UpdateHistoryMenu: up to 10 entries
    Menu->clear();
    int Count = std::min(Back ? Q->BackCount() : Q->ForwardCount(), 10);
    for (int Step = 1; Step <= Count; Step++)
    {
      int Index = Back ? -Step : Step;
      QString Path = Q->HistoryPath(Index);
      QAction * Action = Menu->addAction(MinimizeName(Path, 50));
      Action->setToolTip(Path);
      Action->setStatusTip(Path);
      connect(Action, &QAction::triggered, this, [this, Index]()
      {
        Q->HistoryGo(Index);
        View->setFocus();
      });
    }
  }

  void ToggleHiddenFiles()
  {
    if (SignalConnected(QMetaMethod::fromSignal(&FilePanel::ToggleHiddenFilesRequested)))
    {
      emit Q->ToggleHiddenFilesRequested();
      return;
    }
    // ToggleShowHiddenFiles
    Configuration & Config = Configuration::Instance();
    Config.ShowHiddenFiles = !Config.ShowHiddenFiles;
    for (FilePanel * Panel : AllPanels())
    {
      Panel->SetShowHiddenFiles(Config.ShowHiddenFiles);
    }
  }
};

//---------------------------------------------------------------------------
// FilePanel
//---------------------------------------------------------------------------

FilePanel::FilePanel(bool Remote, QWidget * Parent) :
  QWidget(Parent),
  FRemote(Remote),
  d(new Private(this))
{
  AllPanels().append(this);
  QVBoxLayout * MainLayout = new QVBoxLayout(this);
  MainLayout->setContentsMargins(0, 0, 0, 0);
  MainLayout->setSpacing(0);
  d->Stack = new QStackedWidget(this);
  MainLayout->addWidget(d->Stack);

  // Panel page
  d->PanelPage = new QWidget(d->Stack);
  QVBoxLayout * Layout = new QVBoxLayout(d->PanelPage);
  Layout->setContentsMargins(0, 0, 0, 0);
  Layout->setSpacing(1);

  // Top dock: path toolbar (path combo, history and navigation bands)
  d->TopDock = new QVBoxLayout();
  d->TopDock->setContentsMargins(0, 0, 0, 0);
  d->TopDock->setSpacing(1);
  QWidget * PathToolbar = new QWidget(d->PanelPage);
  QHBoxLayout * PathLayout = new QHBoxLayout(PathToolbar);
  PathLayout->setContentsMargins(2, 1, 2, 0);
  PathLayout->setSpacing(1);
  d->PathCombo = new QComboBox(PathToolbar);
  d->PathCombo->setEditable(true);
  d->PathCombo->setInsertPolicy(QComboBox::NoInsert);
  d->PathCombo->setDuplicatesEnabled(false);
  d->PathCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  d->PathCombo->setMinimumContentsLength(10);
  d->PathCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  if (Remote)
  {
    d->PathCombo->setItemDelegate(new PathComboDelegate(d->PathCombo));
  }
  PathLayout->addWidget(d->PathCombo, 1);
  connect(d->PathCombo, &QComboBox::textActivated, d, [this](const QString & Text)
  {
    if (!Text.trimmed().isEmpty())
    {
      ChangeDirectory(Text);
      d->View->setFocus();
    }
  });
  // A typed path + Enter: with NoInsert QComboBox emits no activation for
  // the edited text, only the line edit reports it
  connect(d->PathCombo->lineEdit(), &QLineEdit::returnPressed, d, [this]()
  {
    QString Text = d->PathCombo->lineEdit()->text();
    if (!Text.trimmed().isEmpty())
    {
      ChangeDirectory(Text);
      d->View->setFocus();
    }
  });

  d->TopDock->addWidget(PathToolbar);
  // Second row: Local/Remote History and Navigation toolbars (DockRow = 1)
  QWidget * ButtonToolbar = new QWidget(d->PanelPage);
  d->ButtonRow = new QHBoxLayout(ButtonToolbar);
  d->ButtonRow->setContentsMargins(2, 0, 2, 0);
  d->ButtonRow->setSpacing(1);

  d->HistoryBand = new QWidget(ButtonToolbar);
  QHBoxLayout * HistoryLayout = new QHBoxLayout(d->HistoryBand);
  HistoryLayout->setContentsMargins(0, 0, 0, 0);
  HistoryLayout->setSpacing(0);
  d->BackButton = d->CreateButton(d->HistoryBand, QStringLiteral("go-previous"), QStyle::SP_ArrowBack,
    QStringLiteral("Back"), &FilePanel::Back);
  d->ForwardButton = d->CreateButton(d->HistoryBand, QStringLiteral("go-next"), QStyle::SP_ArrowForward,
    QStringLiteral("Forward"), &FilePanel::Forward);
  // Back/Forward drop-down history (DropdownCombo)
  d->BackMenu = new QMenu(d->BackButton);
  d->ForwardMenu = new QMenu(d->ForwardButton);
  d->BackButton->setMenu(d->BackMenu);
  d->ForwardButton->setMenu(d->ForwardMenu);
  d->BackButton->setPopupMode(QToolButton::MenuButtonPopup);
  d->ForwardButton->setPopupMode(QToolButton::MenuButtonPopup);
  connect(d->BackMenu, &QMenu::aboutToShow, d, [this]() { d->PopulateHistoryMenu(d->BackMenu, true); });
  connect(d->ForwardMenu, &QMenu::aboutToShow, d, [this]() { d->PopulateHistoryMenu(d->ForwardMenu, false); });
  HistoryLayout->addWidget(d->BackButton);
  HistoryLayout->addWidget(d->ForwardButton);
  d->ButtonRow->addWidget(d->HistoryBand);

  d->NavigationBand = new QWidget(ButtonToolbar);
  QHBoxLayout * NavigationLayout = new QHBoxLayout(d->NavigationBand);
  NavigationLayout->setContentsMargins(0, 0, 0, 0);
  NavigationLayout->setSpacing(0);
  d->ParentButton = d->CreateButton(d->NavigationBand, QStringLiteral("go-up"), QStyle::SP_FileDialogToParent,
    QStringLiteral("Parent Directory"), &FilePanel::ParentDirectory);
  d->RootButton = d->CreateButton(d->NavigationBand, QStringLiteral("go-top"), QStyle::SP_DriveHDIcon,
    QStringLiteral("Root Directory"), &FilePanel::RootDirectory);
  d->HomeButton = d->CreateButton(d->NavigationBand, QStringLiteral("go-home"), QStyle::SP_DirHomeIcon,
    QStringLiteral("Home Directory"), &FilePanel::HomeDirectory);
  d->RefreshButton = d->CreateButton(d->NavigationBand, QStringLiteral("view-refresh"), QStyle::SP_BrowserReload,
    QStringLiteral("Refresh"), &FilePanel::Refresh);
  for (QToolButton * Button : { d->ParentButton, d->RootButton, d->HomeButton, d->RefreshButton })
  {
    NavigationLayout->addWidget(Button);
  }
  d->ButtonRow->addWidget(d->NavigationBand);
  d->ButtonRow->addStretch(1);
  d->TopDock->addWidget(ButtonToolbar);
  Layout->addLayout(d->TopDock);

  // Path label (TPathLabel), colored for the active panel
  d->Label = new PathLabel(d->PanelPage);
  d->Label->OnPathClick = [this](const QString & Path) { ChangeDirectory(Path); };
  d->Label->OnMaskClick = [this]() { emit FilterRequested(); };
  d->Label->OnClick = [this]() { d->View->setFocus(); };
  d->Label->OnDoubleClick = [this]() { emit PathLabelDoubleClicked(); };
  d->Label->OnContextMenu = [this](const QPoint & GlobalPos) { emit PanelMenuRequested(GlobalPos); };
  Layout->addWidget(d->Label);

  d->Model = new FileListModel(Remote, this);
  d->View = new PanelView(d->PanelPage);
  d->View->Handler = d;
  d->View->setModel(d->Model);
  d->Delegate = new PanelDelegate(d->Model, d->View);
  d->View->setItemDelegate(d->Delegate);
  d->View->setRootIsDecorated(false);
  d->View->setItemsExpandable(false);
  d->View->setUniformRowHeights(true);
  d->View->setAllColumnsShowFocus(false);
  d->View->setAlternatingRowColors(false);
  // Marks are kept by the model (Norton-like selection); the view only has a cursor
  d->View->setSelectionMode(QAbstractItemView::NoSelection);
  d->View->setSelectionBehavior(QAbstractItemView::SelectRows);
  d->View->setEditTriggers(QAbstractItemView::NoEditTriggers);
  d->View->setSortingEnabled(true);
  d->View->setDragEnabled(true);
  d->View->setAcceptDrops(true);
  d->View->setDropIndicatorShown(false);
  d->View->setDragDropMode(QAbstractItemView::DragDrop);
  d->View->setDefaultDropAction(Qt::CopyAction);
  d->View->setContextMenuPolicy(Qt::CustomContextMenu);
  d->View->installEventFilter(d);
  d->PathCombo->installEventFilter(d);
  installEventFilter(d);
  connect(d->View, &QWidget::customContextMenuRequested, d, [this](const QPoint & Pos)
  {
    QModelIndex Index = d->View->indexAt(Pos);
    bool OnItem = Index.isValid() && !d->Model->IsParentRow(Index.row());
    emit ContextMenuRequested(d->View->viewport()->mapToGlobal(Pos), OnItem);
  });

  QHeaderView * Header = d->View->header();
  Header->setSectionsMovable(true);
  Header->setStretchLastSection(false);
  Header->setContextMenuPolicy(Qt::CustomContextMenu);
  Header->setSortIndicatorShown(true);
  for (int Column = 0; Column < FileListModel::ColCount; Column++)
  {
    d->ColumnWidths[Column] = FileListModel::DefaultColumnWidth(Column, Remote);
  }
  connect(Header, &QHeaderView::customContextMenuRequested, d, [this](const QPoint & Pos) { d->ShowHeaderMenu(Pos); });
  connect(Header, &QHeaderView::sectionResized, d, [this](int Column, int, int NewSize)
  {
    if ((NewSize > 0) && !d->View->header()->isSectionHidden(Column))
    {
      d->ColumnWidths[Column] = NewSize;
    }
    d->ScheduleSaveParams();
  });
  connect(Header, &QHeaderView::sectionMoved, d, [this]() { d->ScheduleSaveParams(); });
  connect(Header, &QHeaderView::sortIndicatorChanged, d, [this]() { d->ScheduleSaveParams(); });
  Layout->addWidget(d->View, 1);

  // "Reading directory" indicator with Cancel
  d->ReadingBar = new QFrame(d->PanelPage);
  d->ReadingBar->setFrameShape(QFrame::StyledPanel);
  QHBoxLayout * ReadingLayout = new QHBoxLayout(d->ReadingBar);
  ReadingLayout->setContentsMargins(4, 1, 4, 1);
  d->ReadingLabel = new QLabel(d->ReadingBar);
  QPushButton * ReadingCancel = new QPushButton(QStringLiteral("Cancel"), d->ReadingBar);
  ReadingCancel->setFocusPolicy(Qt::NoFocus);
  ReadingLayout->addWidget(d->ReadingLabel, 1);
  ReadingLayout->addWidget(ReadingCancel);
  connect(ReadingCancel, &QPushButton::clicked, d, [this]() { CancelLoading(); d->View->setFocus(); });
  d->ReadingBar->hide();
  Layout->addWidget(d->ReadingBar);

  // Status bar: file info, "N hidden", "N filtered"
  d->StatusBar = new QWidget(d->PanelPage);
  QHBoxLayout * StatusLayout = new QHBoxLayout(d->StatusBar);
  StatusLayout->setContentsMargins(0, 0, 0, 0);
  StatusLayout->setSpacing(2);
  d->StatusLabel = new StatusPanel(d->StatusBar);
  d->HiddenLabel = new StatusPanel(d->StatusBar);
  d->FilteredLabel = new StatusPanel(d->StatusBar);
  for (StatusPanel * Label : { d->StatusLabel, d->HiddenLabel, d->FilteredLabel })
  {
    Label->setContentsMargins(4, 1, 4, 1);
    Label->setFrameShape(QFrame::StyledPanel);
    Label->setFrameShadow(QFrame::Sunken);
    StatusLayout->addWidget(Label, (Label == d->StatusLabel) ? 1 : 0);
  }
  d->StatusLabel->setToolTip(QStringLiteral("File selection info"));
  d->StatusLabel->setMinimumWidth(10);
  d->StatusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  d->HiddenLabel->setToolTip(QStringLiteral("Click to show hidden files"));
  d->HiddenLabel->hide();
  d->FilteredLabel->hide();
  d->StatusLabel->OnClick = [this]() { d->View->setFocus(); };
  d->HiddenLabel->OnClick = [this]() { d->View->setFocus(); d->ToggleHiddenFiles(); };
  d->FilteredLabel->OnClick = [this]() { d->View->setFocus(); emit FilterRequested(); };
  Layout->addWidget(d->StatusBar);

  d->Stack->addWidget(d->PanelPage);

  // "Not connected" page
  d->PlaceholderPage = new QWidget(d->Stack);
  QVBoxLayout * PlaceholderLayout = new QVBoxLayout(d->PlaceholderPage);
  PlaceholderLayout->addStretch(1);
  QLabel * NotConnected = new QLabel(QStringLiteral("Not connected"), d->PlaceholderPage);
  NotConnected->setAlignment(Qt::AlignCenter);
  PlaceholderLayout->addWidget(NotConnected);
  QPushButton * NewSession = new QPushButton(QStringLiteral("New Session..."), d->PlaceholderPage);
  QHBoxLayout * ButtonLayout = new QHBoxLayout();
  ButtonLayout->addStretch(1);
  ButtonLayout->addWidget(NewSession);
  ButtonLayout->addStretch(1);
  PlaceholderLayout->addLayout(ButtonLayout);
  PlaceholderLayout->addStretch(1);
  connect(NewSession, &QPushButton::clicked, this, &FilePanel::NewSessionRequested);
  d->Stack->addWidget(d->PlaceholderPage);
  d->Stack->setCurrentWidget(d->PlaceholderPage);

  // Timers
  d->ReadingTimer = new QTimer(d);
  d->ReadingTimer->setSingleShot(true);
  connect(d->ReadingTimer, &QTimer::timeout, d, [this]() { d->UpdateReadingBar(); });
  d->RenameTimer = new QTimer(d);
  d->RenameTimer->setSingleShot(true);
  connect(d->RenameTimer, &QTimer::timeout, d, [this]()
  {
    if ((d->RenameRow >= 0) && (d->RenameRow == d->CurrentRow()) && d->View->hasFocus())
    {
      StartRename();
    }
  });
  d->WatchTimer = new QTimer(d);
  d->WatchTimer->setSingleShot(true);
  connect(d->WatchTimer, &QTimer::timeout, d, [this]() { d->AutoRefresh(); });
  d->RemoteRefreshTimer = new QTimer(d);
  connect(d->RemoteRefreshTimer, &QTimer::timeout, d, [this]() { d->RemoteRefreshTimeout(); });
  d->SaveParamsTimer = new QTimer(d);
  d->SaveParamsTimer->setSingleShot(true);
  connect(d->SaveParamsTimer, &QTimer::timeout, d, [this]() { d->SavePanelState(); });

  // Model and cursor notifications
  connect(d->Model, &FileListModel::MarksChanged, d, [this]()
  {
    d->UpdateStatus();
    emit SelectionChanged();
  });
  connect(d->View->selectionModel(), &QItemSelectionModel::currentChanged, d, [this]()
  {
    if (d->IncrementalSearching <= 0)
    {
      d->ResetIncrementalSearch();
    }
    emit SelectionChanged();
  });
  connect(d->Model, &FileListModel::RenameRequested, d, [this](const QString & OldName, const QString & NewName)
  {
    emit RenameRequested(FullPath(OldName), NewName);
  });

  d->LoadPanelState();
  d->ApplyConfiguration();
  SetActive(false);
  d->UpdateButtons();
  d->UpdateStatus();
}

FilePanel::~FilePanel()
{
  AllPanels().removeAll(this);
  d->View->Handler = nullptr;
  if (d->SaveParamsTimer->isActive())
  {
    d->SaveParamsTimer->stop();
    d->SavePanelState();
  }
  if ((d->Live() != nullptr) && d->Loading)
  {
    d->Live()->CancelLoad();
  }
}

void FilePanel::SetProvider(DirectoryProvider * Provider)
{
  if ((d->Live() != nullptr) && d->Loading)
  {
    d->Live()->CancelLoad();
  }
  FProvider = Provider;
  d->ProviderAlive = (Provider != nullptr) ? Provider->AliveFlag() : nullptr;
  d->Generation++;
  d->SetLoading(false);
  d->Directory.clear();
  d->BackHistory.clear();
  d->ForwardHistory.clear();
  d->SavedNames.clear();
  d->ResetIncrementalSearch();
  d->Model->Clear();
  {
    QSignalBlocker Blocker(d->PathCombo);
    d->PathCombo->clear();
  }
  d->UpdatePathLabel();
  d->ArmWatcher();
  if (Provider == nullptr)
  {
    d->Stack->setCurrentWidget(d->PlaceholderPage);
  }
  else
  {
    if (LocalDirectoryProvider * Local = dynamic_cast<LocalDirectoryProvider *>(Provider))
    {
      Local->ShowHiddenFiles = d->ShowHidden;
    }
    else if (RemoteDirectoryProvider * RemoteProvider = dynamic_cast<RemoteDirectoryProvider *>(Provider))
    {
      RemoteProvider->ShowHiddenFiles = d->ShowHidden;
    }
    d->Stack->setCurrentWidget(d->PanelPage);
    d->ApplyConfiguration();
    // Open the home directory unless the caller navigates somewhere right away
    quint64 Gen = d->Generation;
    QPointer<FilePanel> Guard(this);
    QTimer::singleShot(0, d, [this, Guard, Gen, Provider]()
    {
      if (!Guard.isNull() && (d->Generation == Gen) && (FProvider == Provider) && d->Directory.isEmpty() && !d->Loading)
      {
        HomeDirectory();
      }
    });
  }
  d->UpdateButtons();
  d->UpdateStatus();
}

QString FilePanel::CurrentDirectory() const
{
  return d->Directory;
}

void FilePanel::ChangeDirectory(const QString & Path)
{
  QString Target = d->NormalizeRequest(Path);
  if (!Target.isEmpty())
  {
    d->Load(Target, Private::LoadRequest());
  }
}

void FilePanel::ChangeDirectory(const QString & Path, const QString & FocusFileName)
{
  QString Target = d->NormalizeRequest(Path);
  if (Target.isEmpty())
  {
    return;
  }
  if ((Target == d->Directory) && !FocusFileName.isEmpty() && FocusFile(FocusFileName))
  {
    return;
  }
  // Refresh when already there, so a file just created (found) gets listed
  Private::LoadRequest Request;
  Request.Kind = (Target == d->Directory) ? Private::lkRefresh : Private::lkChange;
  Request.FocusName = FocusFileName;
  d->Load(Target, Request);
}

bool FilePanel::FocusFile(const QString & FileName)
{
  int Row = d->Model->RowOfName(FileName);
  if (Row < 0)
  {
    return false;
  }
  d->SetCurrentRow(Row);
  return true;
}

QStringList FilePanel::VisitedDirectories() const
{
  QStringList Result;
  auto Add = [&Result](const QString & Path)
  {
    if (!Path.isEmpty() && !Result.contains(Path))
    {
      Result.append(Path);
    }
  };
  for (const QString & Path : d->BackHistory)
  {
    Add(Path);
  }
  Add(d->Directory);
  for (int Index = d->ForwardHistory.size() - 1; Index >= 0; Index--)
  {
    Add(d->ForwardHistory.at(Index));
  }
  return Result;
}

void FilePanel::Refresh()
{
  d->ApplyConfiguration();
  if (!d->Directory.isEmpty())
  {
    Private::LoadRequest Request;
    Request.Kind = Private::lkRefresh;
    // a local directory that has gone: on to the nearest readable parent, as
    // the automatic refresh does
    Request.Silent = !FRemote && !QFileInfo(d->Directory).isDir();
    d->Load(d->Directory, Request);
  }
}

void FilePanel::RefreshAfterOperation()
{
  if (FRemote && !Configuration::Instance().AutoReadDirectoryAfterOp)
  {
    return;
  }
  Refresh();
}

void FilePanel::Back()
{
  HistoryGo(-1);
}

void FilePanel::Forward()
{
  HistoryGo(1);
}

void FilePanel::HistoryGo(int Index)
{
  if ((d->Live() == nullptr) || (Index == 0) ||
      ((Index < 0) && (-Index > BackCount())) || ((Index > 0) && (Index > ForwardCount())))
  {
    return;
  }
  Private::LoadRequest Request;
  Request.Kind = Private::lkHistory;
  Request.HistoryIndex = Index;
  d->Load(HistoryPath(Index), Request);
}

QString FilePanel::HistoryPath(int Index) const
{
  if (Index == 0)
  {
    return d->Directory;
  }
  if (Index < 0)
  {
    int Position = d->BackHistory.size() + Index;
    return ((Position >= 0) && (Position < d->BackHistory.size())) ? d->BackHistory.at(Position) : QString();
  }
  int Position = d->ForwardHistory.size() - Index;
  return ((Position >= 0) && (Position < d->ForwardHistory.size())) ? d->ForwardHistory.at(Position) : QString();
}

int FilePanel::BackCount() const
{
  return d->BackHistory.size();
}

int FilePanel::ForwardCount() const
{
  return d->ForwardHistory.size();
}

void FilePanel::ParentDirectory()
{
  if (d->Directory.isEmpty() || d->IsRootPath(d->Directory))
  {
    return;
  }
  Private::LoadRequest Request;
  Request.FocusName = UnixPath::ExtractFileName(d->Directory);
  d->Load(d->ParentOf(d->Directory), Request);
}

void FilePanel::RootDirectory()
{
  if (d->Live() != nullptr)
  {
    // Windows: the root of the current drive
    d->Load(FRemote ? QStringLiteral("/") : LocalPath::Root(d->Directory.isEmpty() ? QDir::homePath() : d->Directory),
      Private::LoadRequest());
  }
}

void FilePanel::HomeDirectory()
{
  DirectoryProvider * Provider = d->Live();
  if (Provider == nullptr)
  {
    return;
  }
  QPointer<FilePanel> Guard(this);
  Provider->ResolveHomeDirectory([Guard, Provider](const QString & Path)
  {
    if (!Guard.isNull() && (Guard->FProvider == Provider) && !Path.isEmpty())
    {
      Guard->d->Load(Path, Private::LoadRequest());
    }
  });
}

bool FilePanel::CanBack() const
{
  return (FProvider != nullptr) && !d->BackHistory.isEmpty();
}

bool FilePanel::CanForward() const
{
  return (FProvider != nullptr) && !d->ForwardHistory.isEmpty();
}

bool FilePanel::IsLoading() const
{
  return d->Loading;
}

void FilePanel::CancelLoading()
{
  if (!d->Loading)
  {
    return;
  }
  if (d->Live() != nullptr)
  {
    d->Live()->CancelLoad();
  }
  d->Generation++;
  d->SetLoading(false);
  d->PathCombo->setEditText(d->Directory);
  // DIRECTORY_READING_CANCELLED
  d->PostNote(FRemote ? QStringLiteral("Reading of remote directory was cancelled.") :
    QStringLiteral("Reading of directory was cancelled."));
}

QStringList FilePanel::SelectedPaths() const
{
  QStringList Result;
  for (int Row : d->OperationRows())
  {
    Result.append(d->Combine(d->Directory, d->Model->File(Row).FileName));
  }
  return Result;
}

QList<RemoteFile> FilePanel::SelectedFiles() const
{
  QList<RemoteFile> Result;
  for (int Row : d->OperationRows())
  {
    Result.append(d->Model->File(Row));
  }
  return Result;
}

bool FilePanel::HasFocusedFile() const
{
  int Row = d->CurrentRow();
  return (Row >= 0) && !d->Model->IsParentRow(Row);
}

RemoteFile FilePanel::FocusedFile() const
{
  int Row = d->CurrentRow();
  return (Row >= 0) ? d->Model->File(Row) : RemoteFile();
}

QString FilePanel::FullPath(const QString & FileName) const
{
  return d->Combine(d->Directory, FileName);
}

int FilePanel::MarkedCount() const
{
  return d->Model->MarkedCount();
}

QStringList FilePanel::MarkedNames() const
{
  return d->Model->MarkedNames();
}

void FilePanel::SelectCurrentItem()
{
  // TCustomNortonLikeListView::SelectCurrentItem(FocusNext = NortonLike != nlOff)
  int Row = d->CurrentRow();
  if ((Row < 0) && (d->RowCount() > 0))
  {
    Row = 0;
  }
  if (Row < 0)
  {
    return;
  }
  d->ToggleRow(Row);
  if ((d->NortonLike != nlOff) && (Row + 1 < d->RowCount()))
  {
    Row++;
  }
  d->Anchor = Row;
  d->SetCurrentRow(Row);
}

bool FilePanel::SetItemCalculatedSize(const QString & Path, qint64 Size)
{
  QString Name = Path;
  if (Path.contains(QLatin1Char('/')))
  {
    if (d->ParentOf(Path) != d->Directory)
    {
      return false;
    }
    Name = UnixPath::ExtractFileName(Path);
  }
  if (!d->Model->SetCalculatedSize(Name, Size))
  {
    return false;
  }
  // ItemCalculatedSizeUpdated: UpdateStatusBar
  d->UpdateStatus();
  return true;
}

void FilePanel::UnmarkFile(const QString & FileName)
{
  QString Name = FileName;
  if (FileName.contains(QLatin1Char('/')))
  {
    // A file of another directory (the panel was navigated during the operation)
    if (d->ParentOf(FileName) != d->Directory)
    {
      return;
    }
    Name = UnixPath::ExtractFileName(FileName);
  }
  if (d->SavedNames.isEmpty() && (d->Model->MarkedCount() > 0))
  {
    SaveSelectedNames();
  }
  d->Model->UnmarkFile(Name);
  int Row = d->Model->RowOfName(Name);
  if (Row >= 0)
  {
    // MakeProgressVisible
    d->View->scrollTo(d->Model->index(Row, 0));
  }
}

void FilePanel::ClearOperationSelection()
{
  if (d->Model->MarkedCount() > 0)
  {
    if (d->SavedNames.isEmpty())
    {
      SaveSelectedNames();
    }
    d->Model->MarkAll(false);
  }
  d->AllMarkedImplicitly = false;
}

void FilePanel::SaveSelectedNames()
{
  const QStringList Names = d->Model->MarkedNames();
  d->SavedNames = QSet<QString>(Names.begin(), Names.end());
}

void FilePanel::RestoreSelectedNames()
{
  d->Model->SetMarkedNames(d->SavedNames, true, true);
  d->AllMarkedImplicitly = false;
  d->View->setFocus();
}

bool FilePanel::SelectedNamesSaved() const
{
  return !d->SavedNames.isEmpty();
}

void FilePanel::SetShowHiddenFiles(bool Show)
{
  // Local listings always contain the hidden files, so no reload is needed
  d->ShowHidden = Show;
  if (LocalDirectoryProvider * Local = dynamic_cast<LocalDirectoryProvider *>(d->Live()))
  {
    Local->ShowHiddenFiles = Show;
  }
  else if (RemoteDirectoryProvider * RemoteProvider = dynamic_cast<RemoteDirectoryProvider *>(d->Live()))
  {
    RemoteProvider->ShowHiddenFiles = Show;
  }
  if (d->Model->ShowHiddenFiles() == Show)
  {
    return;
  }
  QString Focus = d->FocusedName();
  d->Model->SetShowHiddenFiles(Show);
  d->RestoreFocus(Focus, QString());
  d->UpdateStatus();
}

void FilePanel::SelectAll()
{
  d->Model->MarkAll(true);
  d->AllMarkedImplicitly = false;
}

void FilePanel::ClearSelection()
{
  d->Model->MarkAll(false);
  d->AllMarkedImplicitly = false;
}

void FilePanel::InvertSelection()
{
  d->Model->InvertMarks();
  d->AllMarkedImplicitly = false;
}

void FilePanel::SelectByMask(const QString & Mask, bool Select, bool ApplyToDirectories)
{
  FileMasks Masks;
  try
  {
    Masks = FileMasks(Mask);
  }
  catch (const FileMasksException &)
  {
    return;
  }
  QSet<QString> Names;
  int Rows = d->Model->rowCount();
  for (int Row = 0; Row < Rows; Row++)
  {
    const RemoteFile & File = d->Model->File(Row);
    if (File.IsParentDirectory)
    {
      continue;
    }
    // TDirViewInt::ItemMatchesFilter / TUnixDirView::ItemMatchesFilter
    FileMasks::Params Params;
    Params.Size = File.IsDirectory ? 0 : File.Size;
    Params.Modification = File.Modification;
    bool ImplicitMatch = false;
    bool Matches =
      (Masks.Matches(File.FileName, false, File.IsDirectory, &Params, true, ImplicitMatch) && !ImplicitMatch);
    if (!Matches && File.IsDirectory && ApplyToDirectories)
    {
      Matches = Masks.Matches(File.FileName, false, false, &Params, true, ImplicitMatch) && !ImplicitMatch;
    }
    if (Matches)
    {
      Names.insert(File.FileName);
    }
  }
  d->Model->SetMarkedNames(Names, Select, false);
  d->AllMarkedImplicitly = false;
}

void FilePanel::SelectFiles(const QStringList & Names)
{
  // Exact names (Compare Directories), replacing current marks
  d->Model->SetMarkedNames(QSet<QString>(Names.begin(), Names.end()), true, true);
  d->AllMarkedImplicitly = false;
}

void FilePanel::SetFilterMask(const QString & Mask)
{
  if (Mask == d->Model->FilterMask())
  {
    return;
  }
  QString Focus = d->FocusedName();
  d->Model->SetFilterMask(Mask);
  d->RestoreFocus(Focus, QString());
  d->UpdatePathLabel();
  d->UpdateStatus();
}

QString FilePanel::FilterMask() const
{
  return d->Model->FilterMask();
}

void FilePanel::SelectSameExtension(bool Select)
{
  // TCustomScpExplorerForm::SelectSameExt: mask "*.ext" ("*." without extension), files only
  if (!HasFocusedFile())
  {
    return;
  }
  RemoteFile Focused = FocusedFile();
  if (Focused.IsDirectory)
  {
    return;
  }
  int Dot = Focused.FileName.lastIndexOf(QLatin1Char('.'));
  QString Ext = (Dot > 0) ? Focused.FileName.mid(Dot) : QStringLiteral(".");
  SelectByMask(QStringLiteral("*") + FileMasks::EscapeMask(Ext), Select, false);
}

void FilePanel::StartRename()
{
  if (!HasFocusedFile())
  {
    return;
  }
  d->ResetIncrementalSearch();
  d->RenameTimer->stop();
  QModelIndex Index = d->Model->index(d->CurrentRow(), FileListModel::ColName);
  d->View->scrollTo(Index);
  d->View->setFocus();
  d->View->edit(Index);
}

void FilePanel::SetActive(bool Active)
{
  FActive = Active;
  d->Label->SetActive(Active);
  QFont Font = d->PathCombo->font();
  Font.setBold(Active);
  d->PathCombo->setFont(Font);
}

QTreeView * FilePanel::View() const
{
  return d->View;
}

void FilePanel::FocusView()
{
  d->View->setFocus();
}

//---------------------------------------------------------------------------
// Sorting and columns

void FilePanel::SetSort(int Column, bool Ascending)
{
  if ((Column < 0) || (Column >= FileListModel::ColCount))
  {
    return;
  }
  d->View->sortByColumn(Column, Ascending ? Qt::AscendingOrder : Qt::DescendingOrder);
}

void FilePanel::SortBy(int Column)
{
  // TCustomIEListView::SortBy
  if (Column == d->Model->SortColumn())
  {
    SetSort(Column, !d->Model->SortAscending());
  }
  else
  {
    SetSort(Column, (Column != FileListModel::ColSize) && (Column != FileListModel::ColChanged));
  }
}

int FilePanel::SortColumn() const
{
  return d->Model->SortColumn();
}

bool FilePanel::SortAscending() const
{
  return d->Model->SortAscending();
}

bool FilePanel::IsColumnVisible(int Column) const
{
  return (Column >= 0) && (Column < FileListModel::ColCount) && !d->View->header()->isSectionHidden(Column);
}

void FilePanel::SetColumnVisible(int Column, bool Visible)
{
  if ((Column <= FileListModel::ColName) || (Column >= FileListModel::ColCount) || (IsColumnVisible(Column) == Visible))
  {
    return;
  }
  QHeaderView * Header = d->View->header();
  if (Visible)
  {
    // A shown column is moved to the back (SetVisibleInternal)
    Header->moveSection(Header->visualIndex(Column), Header->count() - 1);
    Header->showSection(Column);
    Header->resizeSection(Column, std::max(d->ColumnWidths[Column], 20));
  }
  else
  {
    Header->hideSection(Column);
  }
  d->ScheduleSaveParams();
}

bool FilePanel::IsColumnAvailable(int Column) const
{
  return (Column >= 0) && (Column < FileListModel::ColCount);
}

void FilePanel::AutoSizeColumns()
{
  for (int Column = 0; Column < FileListModel::ColCount; Column++)
  {
    if (IsColumnVisible(Column))
    {
      d->View->resizeColumnToContents(Column);
    }
  }
  d->ScheduleSaveParams();
}

void FilePanel::ResetColumnLayout()
{
  SetDirViewParams(DefaultDirViewParams(FRemote));
  d->ScheduleSaveParams();
}

QString FilePanel::DefaultDirViewParams(bool Remote)
{
  // ScpCommanderRemotePanelDirViewParamsDefault / ScpCommanderLocalPanelDirViewParamsDefault
  // (local: plus NixSCP's Owner, Group and Link target columns, hidden)
  return Remote ?
    QStringLiteral("0;1;0|150,1;70,1;150,1;79,1;62,1;55,0;20,0;150,0;125,0;@96|6;7;8;0;1;2;3;4;5") :
    QStringLiteral("0;1;0|150,1;70,1;120,1;150,1;55,0;55,0;62,0;55,0;150,0;@96|5;0;1;2;3;4");
}

QString FilePanel::DirViewParams() const
{
  // TIEListViewColProperties::GetParamsStr: "sort;ascending|width,visible;...;@ppi|order"
  QHeaderView * Header = d->View->header();
  const int * Map = d->ColumnMap();
  int WinSCPColumns = d->WinSCPColumns();
  int SortIndex = std::max(d->WinSCPIndexOf(d->Model->SortColumn()), 0);
  QString Result = QStringLiteral("%1;%2|").arg(SortIndex).arg(d->Model->SortAscending() ? 1 : 0);
  QStringList Widths;
  for (int Index = 0; Index < FileListModel::ColCount; Index++)
  {
    int Column = Map[Index];
    bool Visible = !Header->isSectionHidden(Column);
    int Width = Visible ? Header->sectionSize(Column) : d->ColumnWidths[Column];
    QString Entry = QStringLiteral("%1,%2").arg(Width).arg(Visible ? 1 : 0);
    if (Index >= WinSCPColumns)
    {
      // NixSCP-only columns also keep their position (WinSCP ignores these entries)
      Entry += QStringLiteral(",%1").arg(Header->visualIndex(Column));
    }
    Widths.append(Entry);
  }
  Result += Widths.join(QLatin1Char(';')) + QStringLiteral(";@96|");
  QStringList Order;
  for (int Visual = 0; Visual < Header->count(); Visual++)
  {
    int WinSCPIndex = d->WinSCPIndexOf(Header->logicalIndex(Visual));
    if ((WinSCPIndex >= 0) && (WinSCPIndex < WinSCPColumns))
    {
      Order.append(QString::number(WinSCPIndex));
    }
  }
  Result += Order.join(QLatin1Char(';'));
  return Result;
}

void FilePanel::SetDirViewParams(const QString & Params)
{
  QHeaderView * Header = d->View->header();
  const int * Map = d->ColumnMap();
  int WinSCPColumns = d->WinSCPColumns();
  d->HeaderUpdating = true;
  QStringList Parts = Params.split(QLatin1Char('|'));
  // Sort
  int SortColumn = d->Model->SortColumn();
  bool SortAscending = d->Model->SortAscending();
  if (!Parts.isEmpty())
  {
    QStringList Sort = Parts.at(0).split(QLatin1Char(';'));
    bool Ok = false;
    int Index = Sort.value(0).toInt(&Ok);
    if (Ok && (Index >= 0) && (Index < FileListModel::ColCount))
    {
      SortColumn = Map[Index];
    }
    int Ascending = Sort.value(1).toInt(&Ok);
    if (Ok)
    {
      SortAscending = (Ascending != 0);
    }
  }
  // Widths and visibility (scaled from the saved pixels per inch)
  QList<QPair<int, int>> ExtraPositions;
  if (Parts.size() > 1)
  {
    QString WidthsStr = Parts.at(1);
    int PixelsPerInch = 96;
    int At = WidthsStr.indexOf(QLatin1Char('@'));
    if (At >= 0)
    {
      bool Ok = false;
      int Value = WidthsStr.mid(At + 1).toInt(&Ok);
      if (Ok && (Value > 0))
      {
        PixelsPerInch = Value;
      }
      WidthsStr = WidthsStr.left(At);
    }
    QStringList Entries = WidthsStr.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (int Index = 0; (Index < Entries.size()) && (Index < FileListModel::ColCount); Index++)
    {
      QStringList Fields = Entries.at(Index).split(QLatin1Char(','));
      int Column = Map[Index];
      bool Ok = false;
      int Width = Fields.value(0).toInt(&Ok);
      if (Ok && (Width > 0))
      {
        d->ColumnWidths[Column] = std::max(20, Width * 96 / PixelsPerInch);
      }
      bool Visible = (Fields.value(1).toInt() != 0) || (Column == FileListModel::ColName);
      Header->setSectionHidden(Column, !Visible);
      if (Visible)
      {
        Header->resizeSection(Column, d->ColumnWidths[Column]);
      }
      if ((Index >= WinSCPColumns) && (Fields.size() > 2))
      {
        int Position = Fields.at(2).toInt(&Ok);
        if (Ok)
        {
          ExtraPositions.append(qMakePair(Position, Column));
        }
      }
    }
  }
  // Order
  if (Parts.size() > 2)
  {
    QList<int> Order;
    for (const QString & Item : Parts.at(2).split(QLatin1Char(';'), Qt::SkipEmptyParts))
    {
      bool Ok = false;
      int Index = Item.toInt(&Ok);
      if (Ok && (Index >= 0) && (Index < WinSCPColumns) && !Order.contains(Map[Index]))
      {
        Order.append(Map[Index]);
      }
    }
    for (int Index = 0; Index < FileListModel::ColCount; Index++)
    {
      if (!Order.contains(Map[Index]))
      {
        Order.append(Map[Index]);
      }
    }
    for (int Visual = 0; Visual < Order.size(); Visual++)
    {
      int Current = Header->visualIndex(Order.at(Visual));
      if (Current != Visual)
      {
        Header->moveSection(Current, Visual);
      }
    }
    std::sort(ExtraPositions.begin(), ExtraPositions.end());
    for (const auto & Extra : ExtraPositions)
    {
      int Target = std::clamp(Extra.first, 0, Header->count() - 1);
      Header->moveSection(Header->visualIndex(Extra.second), Target);
    }
  }
  Header->setSectionHidden(FileListModel::ColName, false);
  d->View->sortByColumn(SortColumn, SortAscending ? Qt::AscendingOrder : Qt::DescendingOrder);
  d->HeaderUpdating = false;
}

//---------------------------------------------------------------------------
// Panel parts

void FilePanel::SetStatusBarVisible(bool Visible)
{
  if (d->StatusBarVisible != Visible)
  {
    d->StatusBarVisible = Visible;
    d->StatusBar->setVisible(Visible);
    d->SavePanelState();
  }
}

bool FilePanel::IsStatusBarVisible() const
{
  return d->StatusBarVisible;
}

void FilePanel::SetHistoryButtonsVisible(bool Visible)
{
  d->HistoryBand->setVisible(Visible);
}

void FilePanel::SetNavigationButtonsVisible(bool Visible)
{
  d->NavigationBand->setVisible(Visible);
}

void FilePanel::AddToolbar(QWidget * Toolbar, bool NewRow)
{
  Toolbar->setParent(d->PanelPage);
  if (NewRow)
  {
    d->TopDock->addWidget(Toolbar);
  }
  else
  {
    // Before the trailing stretch of the History/Navigation row
    d->ButtonRow->insertWidget(d->ButtonRow->count() - 1, Toolbar);
  }
}

void FilePanel::FocusPathCombo()
{
  d->PathCombo->setFocus(Qt::ShortcutFocusReason);
  if (d->PathCombo->lineEdit() != nullptr)
  {
    d->PathCombo->lineEdit()->selectAll();
  }
}

void FilePanel::ShowPathComboPopup()
{
  d->PathCombo->setFocus(Qt::ShortcutFocusReason);
  d->PathCombo->showPopup();
}

void FilePanel::ApplyConfiguration()
{
  d->ApplyConfiguration();
}

void FilePanel::SetRemoteDragDataFactory(std::function<QMimeData *(const QStringList & RemotePaths)> Factory)
{
  d->RemoteDragFactory = std::move(Factory);
}
