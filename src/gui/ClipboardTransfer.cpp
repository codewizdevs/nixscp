// Files on the clipboard and in drags to other applications (see
// ClipboardTransfer.h). Ports of forms/CustomScpExplorer.cpp
// (CopyFilesToClipboard, PasteFromClipBoard, DDDownload, DoWarnLackOfTempSpace)
// and core/Terminal.cpp + core/ScpFileSystem.cpp CopyFile (remote duplicate).
#include "gui/ClipboardTransfer.h"
#include "gui/CopyDialog.h"
#include "gui/DesktopIntegration.h"
#include "gui/GuiUserInterface.h"
#include "gui/ProgressDialog.h"
#include "core/Configuration.h"
#include "core/FtpFileSystem.h"
#include "core/RemoteFile.h"
#include "core/S3FileSystem.h"
#include "core/SecureShell.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QUrl>

namespace ClipboardTransfer
{

QString UriListMimeType() { return QStringLiteral("text/uri-list"); }
QString GnomeCopiedFilesMimeType() { return QStringLiteral("x-special/gnome-copied-files"); }
QString KdeCutSelectionMimeType() { return QStringLiteral("application/x-kde-cutselection"); }
QString FilesMimeType() { return QStringLiteral("application/x-nixscp-files"); }

static QByteArray UriList(const QStringList & Paths)
{
  QByteArray Result;
  for (const QString & Path : Paths)
  {
    Result += QUrl::fromLocalFile(Path).toEncoded() + "\r\n";
  }
  return Result;
}

static QByteArray GnomeCopiedFiles(const QStringList & Paths, bool Cut)
{
  QByteArray Result = Cut ? "cut" : "copy";
  for (const QString & Path : Paths)
  {
    Result += '\n' + QUrl::fromLocalFile(Path).toEncoded();
  }
  return Result;
}

static QByteArray FilesJson(const QStringList & Paths, bool Remote)
{
  QJsonObject Obj;
  Obj.insert(QStringLiteral("remote"), Remote);
  Obj.insert(QStringLiteral("paths"), QJsonArray::fromStringList(Paths));
  return QJsonDocument(Obj).toJson(QJsonDocument::Compact);
}

//---------------------------------------------------------------------------
// Local files

QMimeData * CreateLocalMimeData(const QStringList & Paths, bool Cut)
{
  QMimeData * Data = new QMimeData();
  QList<QUrl> Urls;
  for (const QString & Path : Paths)
  {
    Urls.append(QUrl::fromLocalFile(Path));
  }
  Data->setUrls(Urls);
  Data->setData(GnomeCopiedFilesMimeType(), GnomeCopiedFiles(Paths, Cut));
  Data->setData(KdeCutSelectionMimeType(), Cut ? "1" : "0");
  Data->setData(FilesMimeType(), FilesJson(Paths, false));
  Data->setText(Paths.join(QLatin1Char('\n')));
  return Data;
}

void CopyLocalFiles(const QStringList & Paths, bool Cut)
{
  if (Paths.isEmpty())
  {
    return;
  }
  QApplication::clipboard()->setMimeData(CreateLocalMimeData(Paths, Cut));
}

//---------------------------------------------------------------------------
// Remote files

static RemoteFilesMimeData::Downloader & DownloaderHook()
{
  static RemoteFilesMimeData::Downloader Instance;
  return Instance;
}

void RemoteFilesMimeData::SetDownloader(const Downloader & ADownloader)
{
  DownloaderHook() = ADownloader;
}

// Temporary folders of drag & drop / clipboard downloads; removed when the
// application quits (the target application may still be copying from them
// after the drop, so not earlier)
static QStringList & TemporaryFolders()
{
  static QStringList Folders;
  static bool Registered = false;
  if (!Registered && (QCoreApplication::instance() != nullptr))
  {
    Registered = true;
    QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, []()
    {
      for (const QString & Folder : TemporaryFolders())
      {
        QDir(Folder).removeRecursively();
      }
      TemporaryFolders().clear();
    });
  }
  return Folders;
}

RemoteFilesMimeData::RemoteFilesMimeData(Terminal * Term, const QStringList & RemotePaths, QWidget * DialogParent,
  bool DragDrop, qint64 TotalSize) :
  FTerminal(Term), FPaths(RemotePaths), FParent(DialogParent), FDragDrop(DragDrop), FTotalSize(TotalSize)
{
  setData(FilesMimeType(), FilesJson(RemotePaths, true));
}

RemoteFilesMimeData::~RemoteFilesMimeData() = default;

QStringList RemoteFilesMimeData::formats() const
{
  QStringList Result = QMimeData::formats();
  for (const QString & Format : {UriListMimeType(), GnomeCopiedFilesMimeType(), QStringLiteral("text/plain")})
  {
    if (!Result.contains(Format))
    {
      Result.append(Format);
    }
  }
  return Result;
}

bool RemoteFilesMimeData::hasFormat(const QString & MimeType) const
{
  return formats().contains(MimeType);
}

QVariant RemoteFilesMimeData::retrieveData(const QString & MimeType, QMetaType Type) const
{
  if (MimeType == QStringLiteral("text/plain"))
  {
    // the remote paths, without downloading
    return FPaths.join(QLatin1Char('\n'));
  }
  if ((MimeType == UriListMimeType()) || (MimeType == GnomeCopiedFilesMimeType()))
  {
    QStringList Files = DownloadedFiles();
    if (Files.isEmpty())
    {
      return QVariant();
    }
    if (MimeType == GnomeCopiedFilesMimeType())
    {
      return GnomeCopiedFiles(Files, false);
    }
    if (Type.id() == QMetaType::QVariantList)
    {
      QVariantList Urls;
      for (const QString & File : Files)
      {
        Urls.append(QUrl::fromLocalFile(File));
      }
      return Urls;
    }
    return UriList(Files);
  }
  return QMimeData::retrieveData(MimeType, Type);
}

QStringList RemoteFilesMimeData::DownloadedFiles() const
{
  if (!FAttempted && !FDownloading)
  {
    FDownloading = true;
    bool Ok = Download();
    FDownloading = false;
    FAttempted = true;
    if (!Ok)
    {
      FLocalFiles.clear();
    }
  }
  return FLocalFiles;
}

bool RemoteFilesMimeData::Download() const
{
  Terminal * Term = FTerminal;
  if ((Term == nullptr) || !Term->Active() || FPaths.isEmpty())
  {
    QApplication::beep();
    return false;
  }
  QWidget * Parent = FParent;
  if (Term->Busy() && !DownloaderHook())
  {
    MessageDialog(Parent, QStringLiteral("**Cannot download the files now.**\n\nThe session is busy with another operation."),
      qtWarning, qaOK);
    return false;
  }
  Configuration & Config = Configuration::Instance();

  // FDragTempDir = WinConfiguration->TemporaryDir(): DDTemporaryDirectory or the system one
  QString Base = Config.DDTemporaryDirectory.trimmed();
  Base = Base.isEmpty() ? QDir::tempPath() : SessionData::ExpandEnvironmentVariables(Base);
  QDir().mkpath(Base);

  // DoWarnLackOfTempSpace
  if (Config.DDWarnLackOfTempSpace && (FTotalSize > 0))
  {
    QStorageInfo Storage(Base);
    qint64 Free = Storage.isValid() ? Storage.bytesAvailable() : -1;
    // DDWarnLackOfTempSpaceRatio
    if ((Free >= 0) && (Free < static_cast<qint64>(FTotalSize * 1.1)))
    {
      MessageParams Params;
      Params.Params = qpNeverAskAgainCheck;
      unsigned int Answer = MessageDialog(Parent,
        QStringLiteral("Too little space on temporary drive!\n\nWhen dragging files from remote directory, files are "
          "downloaded first to temporary directory '%1'. There are %2 free on the drive. Total size of dragged files is %3.\n\n"
          "Note: Temporary directory can be changed from Preferences window.\n\nDo you still want to download files?")
          .arg(Base, QLocale().formattedDataSize(Free), QLocale().formattedDataSize(FTotalSize)),
        qtWarning, qaYes | qaNo, QStringLiteral("ui_pref_dragdrop#temporary"), &Params);
      if (Answer == qaNeverAskAgain)
      {
        Config.DDWarnLackOfTempSpace = false;
        Config.Save();
      }
      if ((Answer != qaYes) && (Answer != qaNeverAskAgain))
      {
        return false;
      }
    }
  }

  QTemporaryDir Temp(QDir(Base).filePath(QStringLiteral("nixscp-dd-XXXXXX")));
  if (!Temp.isValid())
  {
    ShowErrorMessage(Parent, QStringLiteral("Cannot create temporary directory '%1'.").arg(Base), Temp.errorString());
    return false;
  }
  Temp.setAutoRemove(false);
  QString Target = Temp.path();
  TemporaryFolders().append(Target);
  FTemporaryDirectory = Target;

  bool Ok = false;
  if (DownloaderHook())
  {
    Ok = DownloaderHook()(Term, FPaths, Target);
  }
  else
  {
    // CopyParamDialog(tdToLocal, ttCopy, Temp = true, ..., DragDrop)
    CopyParam Param = Config.DefaultCopyParam();
    QStringList Names;
    for (const QString & Path : FPaths)
    {
      Names.append(UnixPath::ExtractFileName(Path));
    }
    DesktopIntegration::FlashOnBackground(Parent);
    CopyDialog Dialog(false, false, Names, Target, Param, Parent);
    Dialog.SetOptions(CopyDialog::coTemp | CopyDialog::coDisableQueue | CopyDialog::coDoNotShowAgain);
    Dialog.SetDragDrop(FDragDrop);
    if (Dialog.exec() != QDialog::Accepted)
    {
      return false;
    }
    Param = Dialog.Param();
    Param.Queue = false;

    QPointer<Terminal> Guard(Term);
    auto * Progress = new ProgressDialog(Term->Progress(), Parent);
    Progress->SetAllowMoveToQueue(false);
    Progress->Start();
    QEventLoop Loop;
    bool Finished = false;
    OperationResult Result;
    QMetaObject::Connection Destroyed = QObject::connect(Term, &QObject::destroyed, &Loop, [&]()
    {
      Result.Ok = false;
      Result.Aborted = true;
      Finished = true;
      Loop.quit();
    });
    Term->CopyToLocal(FPaths, Target, Param, false, [&](const OperationResult & AResult)
    {
      Result = AResult;
      Finished = true;
      Loop.quit();
    });
    if (!Finished)
    {
      Loop.exec(QEventLoop::AllEvents);
    }
    QObject::disconnect(Destroyed);
    Progress->Finish(Result.Ok);
    Progress->deleteLater();
    if (!Result.Ok && !Result.Aborted)
    {
      ShowErrorMessage(Parent, Result.Message, Result.MoreMessages);
    }
    Ok = Result.Ok;
  }
  if (!Ok)
  {
    return false;
  }
  const QFileInfoList Entries = QDir(Target).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
    QDir::Name);
  for (const QFileInfo & Entry : Entries)
  {
    FLocalFiles.append(Entry.absoluteFilePath());
  }
  return !FLocalFiles.isEmpty();
}

RemoteFilesMimeData * CreateRemoteMimeData(Terminal * Term, const QStringList & RemotePaths, QWidget * DialogParent,
  bool DragDrop, qint64 TotalSize)
{
  if ((Term == nullptr) || RemotePaths.isEmpty())
  {
    return nullptr;
  }
  return new RemoteFilesMimeData(Term, RemotePaths, DialogParent, DragDrop, TotalSize);
}

void CopyRemoteFiles(Terminal * Term, const QStringList & RemotePaths, QWidget * DialogParent, qint64 TotalSize)
{
  RemoteFilesMimeData * Data = CreateRemoteMimeData(Term, RemotePaths, DialogParent, false, TotalSize);
  if (Data != nullptr)
  {
    QApplication::clipboard()->setMimeData(Data);
  }
}

//---------------------------------------------------------------------------
// Paste

static QStringList LocalPathsOf(const QList<QUrl> & Urls)
{
  QStringList Result;
  for (const QUrl & Url : Urls)
  {
    if (Url.isLocalFile())
    {
      Result.append(QDir::cleanPath(Url.toLocalFile()));
    }
  }
  return Result;
}

Content Read(const QMimeData * Mime)
{
  Content Result;
  if (Mime == nullptr)
  {
    Mime = QApplication::clipboard()->mimeData();
  }
  if (Mime == nullptr)
  {
    return Result;
  }
  if (const RemoteFilesMimeData * Remote = qobject_cast<const RemoteFilesMimeData *>(Mime))
  {
    Result.Type = Content::ckRemoteFiles;
    Result.Paths = Remote->RemotePaths();
    Result.Source = Remote->SourceTerminal();
    Result.Remote = const_cast<RemoteFilesMimeData *>(Remote);
    return Result;
  }
  if (Mime->hasFormat(GnomeCopiedFilesMimeType()))
  {
    // "copy" or "cut", then one URI per line
    QList<QByteArray> Lines = Mime->data(GnomeCopiedFilesMimeType()).split('\n');
    if (!Lines.isEmpty())
    {
      QByteArray Operation = Lines.takeFirst().trimmed();
      QList<QUrl> Urls;
      for (const QByteArray & Line : Lines)
      {
        if (!Line.trimmed().isEmpty())
        {
          Urls.append(QUrl::fromEncoded(Line.trimmed()));
        }
      }
      Result.Paths = LocalPathsOf(Urls);
      if (!Result.Paths.isEmpty())
      {
        Result.Type = Content::ckLocalFiles;
        Result.Cut = (Operation == "cut");
        return Result;
      }
    }
  }
  if (Mime->hasUrls())
  {
    Result.Paths = LocalPathsOf(Mime->urls());
    if (!Result.Paths.isEmpty())
    {
      Result.Type = Content::ckLocalFiles;
      Result.Cut = Mime->hasFormat(KdeCutSelectionMimeType()) && Mime->data(KdeCutSelectionMimeType()).startsWith('1');
      return Result;
    }
  }
  if (Mime->hasText())
  {
    QString Text = Mime->text().trimmed();
    if (!Text.isEmpty())
    {
      Result.Type = Content::ckText;
      Result.Text = Text;
    }
  }
  return Result;
}

bool CanPaste(const QMimeData * Mime)
{
  Content C = Read(Mime);
  switch (C.Type)
  {
    case Content::ckLocalFiles:
    case Content::ckRemoteFiles:
      return true;
    case Content::ckText:
      return !C.Text.contains(QLatin1Char('\n'));
    default:
      return false;
  }
}

TextKind ClassifyText(const QString & AText, bool RemotePanel, QString * Value)
{
  QString Text = AText.trimmed();
  if (Text.isEmpty() || Text.contains(QLatin1Char('\n')))
  {
    return tkNone;
  }
  // StoredSessions->IsUrl: a session URL opens a new session
  static const char * const Schemes[] =
    {"sftp://", "scp://", "ftp://", "ftps://", "ftpes://", "dav://", "davs://", "http://", "https://", "s3://", "ssh://", "webdav://"};
  for (const char * Scheme : Schemes)
  {
    if (Text.startsWith(QLatin1String(Scheme), Qt::CaseInsensitive))
    {
      if (Value != nullptr)
      {
        *Value = Text;
      }
      return tkSessionUrl;
    }
  }
  if (RemotePanel)
  {
    if (Text.startsWith(QLatin1Char('/')) || Text.startsWith(QLatin1Char('~')))
    {
      if (Value != nullptr)
      {
        *Value = Text;
      }
      return tkPath;
    }
    return tkNone;
  }
  QString Path = Text;
  if (Path.startsWith(QStringLiteral("file://")))
  {
    Path = QUrl(Path).toLocalFile();
  }
  else if ((Path == QStringLiteral("~")) || Path.startsWith(QStringLiteral("~/")))
  {
    Path = QDir::homePath() + Path.mid(1);
  }
  if (!QDir::isAbsolutePath(Path))
  {
    return tkNone;
  }
  QFileInfo Info(Path);
  if (!Info.exists())
  {
    return tkNone;
  }
  if (Value != nullptr)
  {
    *Value = QDir::cleanPath(Info.absoluteFilePath());
  }
  return tkPath;
}

//---------------------------------------------------------------------------
// Remote duplicate

bool CanDuplicateRemote(Terminal * Term)
{
  if ((Term == nullptr) || !Term->Active())
  {
    return false;
  }
  TFSProtocol Protocol = Term->Data().FSProtocol;
  return ((Protocol == fsFTP) && Term->IsCapable(fcRemoteCopy)) ||
    (Protocol == fsS3) ||
    ((Protocol != fsFTP) && (Protocol != fsWebDAV) && Term->CanAnyCommand());
}

static QString DuplicateRemoteName(FileSystem & Fs, const QString & Directory, const QString & FileName)
{
  QString Base = FileName;
  QString Extension;
  int Dot = FileName.lastIndexOf(QLatin1Char('.'));
  if (Dot > 0)
  {
    Base = FileName.left(Dot);
    Extension = FileName.mid(Dot);
  }
  for (int Index = 1; Index < 1000; Index++)
  {
    QString Name = (Index == 1) ? QStringLiteral("%1 (copy)%2").arg(Base, Extension) :
      QStringLiteral("%1 (copy %2)%3").arg(Base).arg(Index).arg(Extension);
    if (!Fs.FileExists(UnixPath::Combine(Directory, Name), nullptr))
    {
      return Name;
    }
  }
  throw ExtException(QStringLiteral("Cannot find a free name for a copy of '%1'.").arg(FileName));
}

void DuplicateRemoteFiles(Terminal * Term, const QStringList & Paths, const QString & TargetDirectory,
  Terminal::Callback Done, bool DuplicateIfSame)
{
  if (Term == nullptr)
  {
    return;
  }
  Term->RunOnWorker([Paths, TargetDirectory, DuplicateIfSame](FileSystem & Fs, FileOperationProgress & Progress, UserInterface & UI)
  {
    Progress.Start(FileOperationProgress::Copy, FileOperationProgress::ToRemote, static_cast<int>(Paths.size()));
    bool YesToAll = false;
    for (const QString & Source : Paths)
    {
      Progress.CheckCancel();
      QString Name = UnixPath::ExtractFileName(Source);
      QString Target = UnixPath::Combine(TargetDirectory, Name);
      if (Target == Source)
      {
        if (!DuplicateIfSame)
        {
          continue;
        }
        Target = UnixPath::Combine(TargetDirectory, DuplicateRemoteName(Fs, TargetDirectory, Name));
      }
      if (TargetDirectory.startsWith(Source + QLatin1Char('/')))
      {
        throw ExtException(QStringLiteral("Cannot move or copy folder '%1' into itself.").arg(Source));
      }
      Progress.SetFile(Source, 0);
      RemoteFile Existing;
      if (!YesToAll && Fs.FileExists(Target, &Existing))
      {
        // FILE_OVERWRITE
        QueryUserParams Params;
        Params.Aliases.append(QueryButtonAlias::YesToAllGroupedWithYes());
        unsigned int Answer = UI.QueryUserEx(
          QStringLiteral("**File '%1' already exists. Overwrite?**").arg(UnixPath::ExtractFileName(Target)), QString(),
          qaYes | qaYesToAll | qaNo | qaCancel, Params);
        if (Answer == qaYesToAll)
        {
          YesToAll = true;
        }
        else if (Answer == qaNo)
        {
          continue;
        }
        else if (Answer != qaYes)
        {
          throw AbortException();
        }
      }
      if (FtpFileSystem * Ftp = dynamic_cast<FtpFileSystem *>(&Fs))
      {
        Ftp->CopyFile(Source, Target);
      }
      else if (S3FileSystem * S3 = dynamic_cast<S3FileSystem *>(&Fs))
      {
        S3->CopyFile(Source, Target);
      }
      else
      {
        AnyCommandFileSystem * Shell = dynamic_cast<AnyCommandFileSystem *>(&Fs);
        if (Shell == nullptr)
        {
          throw ExtException(QStringLiteral("Operation not supported."));
        }
        auto Run = [&](const QString & AdditionalSwitches, QStringList & ErrorOutput)
        {
          // fsCopyFile: cp -p -r -f %s "%s" "%s"
          QString Command = QStringLiteral("cp -p -r -f ") +
            (AdditionalSwitches.isEmpty() ? QString() : (AdditionalSwitches + QLatin1Char(' '))) +
            ShellQuoteStr(Source) + QLatin1Char(' ') + ShellQuoteStr(Target);
          return Shell->AnyCommand(Command, QString(),
            [&](const QString & Line, bool StdError) { if (StdError) ErrorOutput.append(Line); }, Progress, nullptr);
        };
        QStringList ErrorOutput;
        // -T (no target directory) is a GNU extension; retry without it
        int ExitCode = Run(QStringLiteral("-T"), ErrorOutput);
        if (ExitCode != 0)
        {
          QStringList ErrorOutput2;
          ExitCode = Run(QString(), ErrorOutput2);
          if (ExitCode != 0)
          {
            // COPY_FILE_ERROR
            throw ExtException(QStringLiteral("Error copying file '%1' to '%2'.").arg(Source, Target),
              (ErrorOutput2.isEmpty() ? ErrorOutput : ErrorOutput2).join(QLatin1Char('\n')));
          }
        }
      }
      Progress.FileFinished();
    }
    Progress.Stop();
  }, std::move(Done));
}

} // namespace ClipboardTransfer
