// The file operations behind the Properties dialog and the Files menu
// (source/forms/CustomScpExplorer.cpp: RemoteDirViewProperties,
// SetProperties, CalculateSize, CalculateChecksum, CreateDirectory,
// RemoteTransferFiles, AddEditLink, CalculateDirectorySizes;
// source/forms/ScpCommander.cpp: local links, Linux symlink(2)/link(2))
#include "gui/FileOperations.h"

#include "core/Configuration.h"
#include "core/CopyParam.h"
#include "core/FileMasks.h"
#include "core/Synchronize.h"
#include "gui/GuiUserInterface.h"
#include "gui/LinkDialog.h"
#include "gui/ProgressDialog.h"
#include "gui/PropertiesDialog.h"

#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QTemporaryDir>
#include <QThread>

#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <memory>

namespace
{

const QString AnyMask = QStringLiteral("*.*");

QString ErrnoText()
{
  return QString::fromLocal8Bit(strerror(errno));
}

QString AbsoluteRemotePath(const QString & Directory, const QString & Path)
{
  if (Path.startsWith(QLatin1Char('/')))
  {
    return Path;
  }
  return Path.isEmpty() ? Directory : UnixPath::Combine(Directory, Path);
}

QString UnixExcludeTrailingSlash(const QString & Path)
{
  QString Result = Path;
  while ((Result.size() > 1) && Result.endsWith(QLatin1Char('/')))
  {
    Result.chop(1);
  }
  return Result;
}

// Starts a remote operation with Run (the main window's progress handling)
// or with an own progress window and error message
void RunOperation(QWidget * Parent, Terminal * Term, const FileOperations::Runner & Run,
  const std::function<void(Terminal::Callback)> & Start, const Terminal::Callback & Done)
{
  if (Run)
  {
    Run(Start, Done);
    return;
  }
  QPointer<QWidget> GuardParent(Parent);
  auto * Progress = new ProgressDialog(Term->Progress(), Parent);
  Progress->SetAllowMoveToQueue(false);
  Progress->Start();
  QPointer<ProgressDialog> GuardProgress(Progress);
  Start([GuardParent, GuardProgress, Done](const OperationResult & Result)
  {
    if (GuardProgress)
    {
      GuardProgress->Finish(Result.Ok);
      GuardProgress->deleteLater();
    }
    if (!Result.Ok && !Result.Aborted && !Result.Fatal)
    {
      ShowErrorMessage(GuardParent, Result.Message, Result.MoreMessages);
    }
    if (Done)
    {
      Done(Result);
    }
  });
}

// Runs a remote operation and waits for it in a nested event loop
OperationResult WaitFor(QWidget * Parent, Terminal * Term, const std::function<void(Terminal::Callback)> & Start,
  bool ShowProgress)
{
  QEventLoop Loop;
  auto Finished = std::make_shared<bool>(false);
  auto Result = std::make_shared<OperationResult>();
  QObject::connect(Term, &QObject::destroyed, &Loop, [&Loop, Finished, Result]()
  {
    Result->Ok = false;
    Result->Fatal = true;
    *Finished = true;
    Loop.quit();
  });
  ProgressDialog * Progress = nullptr;
  if (ShowProgress)
  {
    Progress = new ProgressDialog(Term->Progress(), Parent);
    Progress->SetAllowMoveToQueue(false);
    Progress->SetAllowSkip(false);
    Progress->Start();
  }
  QPointer<QEventLoop> GuardLoop(&Loop);
  Start([GuardLoop, Finished, Result](const OperationResult & R)
  {
    *Result = R;
    *Finished = true;
    if (GuardLoop)
    {
      GuardLoop->quit();
    }
  });
  if (!*Finished)
  {
    Loop.exec();
  }
  if (Progress != nullptr)
  {
    Progress->Finish(Result->Ok);
    Progress->deleteLater();
  }
  return *Result;
}

// Runs Job on a worker thread and waits in a nested event loop with a
// progress window on Progress (local operations)
void RunLocal(QWidget * Parent, FileOperationProgress & Progress, int Count, const std::function<void()> & Job)
{
  Progress.Start(FileOperationProgress::Calculate, FileOperationProgress::ToLocal, Count);
  auto * Dialog = new ProgressDialog(Progress, Parent);
  Dialog->SetAllowMoveToQueue(false);
  Dialog->SetAllowSkip(false);
  Dialog->Start();
  QEventLoop Loop;
  QThread * Thread = QThread::create(Job);
  QObject::connect(Thread, &QThread::finished, &Loop, &QEventLoop::quit);
  Thread->start();
  Loop.exec();
  Thread->wait();
  delete Thread;
  Progress.Stop();
  Dialog->Finish(!Progress.Cancelled());
  Dialog->deleteLater();
}

bool ResolveLocalToken(const QString & AText, bool User, unsigned int & Id)
{
  // "name [id]", "[id]", a number or a name
  QString Text = AText.trimmed();
  bool Ok = false;
  qsizetype IDStart = Text.lastIndexOf(QLatin1Char('['));
  if ((IDStart >= 0) && Text.endsWith(QLatin1Char(']')))
  {
    Id = Text.mid(IDStart + 1, Text.size() - IDStart - 2).toUInt(&Ok);
    return Ok;
  }
  QByteArray Name = Text.toLocal8Bit();
  if (User)
  {
    if (struct passwd * Pw = getpwnam(Name.constData()))
    {
      Id = Pw->pw_uid;
      return true;
    }
  }
  else if (struct group * Gr = getgrnam(Name.constData()))
  {
    Id = Gr->gr_gid;
    return true;
  }
  Id = Text.toUInt(&Ok);
  return Ok;
}

// Local counterpart of Terminal::ChangeFilesProperties (chmod/chown of the
// files, recursively); returns the errors
QStringList ChangeLocalProperties(const QStringList & Paths, const RemoteProperties & Properties)
{
  QStringList Errors;
  unsigned int Uid = static_cast<unsigned int>(-1), Gid = static_cast<unsigned int>(-1);
  if (!Properties.Owner.isEmpty() && !ResolveLocalToken(Properties.Owner, true, Uid))
  {
    return {QStringLiteral("Enter valid owner name.")};
  }
  if (!Properties.Group.isEmpty() && !ResolveLocalToken(Properties.Group, false, Gid))
  {
    return {QStringLiteral("Enter valid group name.")};
  }
  std::function<void(const QString &)> Change = [&](const QString & Path)
  {
    QByteArray Name = QFile::encodeName(Path);
    struct stat St = {};
    if (lstat(Name.constData(), &St) != 0)
    {
      Errors << QStringLiteral("Can't change properties of file '%1'.\n%2").arg(Path, ErrnoText());
      return;
    }
    if (S_ISLNK(St.st_mode))
    {
      return; // chmod/chown would change the target
    }
    bool Directory = S_ISDIR(St.st_mode);
    if (Properties.Recursive && Directory)
    {
      QDir Dir(Path);
      const QStringList Entries = Dir.entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
      for (const QString & Entry : Entries)
      {
        Change(Dir.filePath(Entry));
      }
    }
    if (!Properties.Owner.isEmpty() || !Properties.Group.isEmpty())
    {
      if (chown(Name.constData(), static_cast<uid_t>(Uid), static_cast<gid_t>(Gid)) != 0)
      {
        Errors << QStringLiteral("Can't change properties of file '%1'.\n%2").arg(Path, ErrnoText());
      }
    }
    if (Properties.ValidRights)
    {
      unsigned int Mode = Properties.CombineRights(St.st_mode & 07777, Directory);
      if (chmod(Name.constData(), Mode) != 0)
      {
        Errors << QStringLiteral("Can't change properties of file '%1'.\n%2").arg(Path, ErrnoText());
      }
    }
  };
  for (const QString & Path : Paths)
  {
    Change(Path);
  }
  return Errors;
}

void LocalUsersGroups(QStringList & Users, QStringList & Groups)
{
  // the current user and its groups
  if (struct passwd * Pw = getpwuid(getuid()))
  {
    Users << QString::fromLocal8Bit(Pw->pw_name);
  }
  int Count = getgroups(0, nullptr);
  if (Count > 0)
  {
    std::vector<gid_t> Ids(static_cast<size_t>(Count));
    Count = getgroups(Count, Ids.data());
    for (int Index = 0; Index < Count; Index++)
    {
      if (struct group * Gr = getgrgid(Ids[static_cast<size_t>(Index)]))
      {
        Groups << QString::fromLocal8Bit(Gr->gr_name);
      }
    }
  }
}

void AddUnique(QStringList & List, const QString & Value)
{
  if (!Value.isEmpty() && !List.contains(Value))
  {
    List << Value;
  }
}

qint64 LocalSize(const QString & Path, Terminal::CalculateSizeStats & Stats, const FileOperationProgress & Progress)
{
  QFileInfo Info(Path);
  if (Info.isSymLink())
  {
    Stats.SymLinks++;
  }
  if (!Info.isDir() || Info.isSymLink())
  {
    if (Info.isDir())
    {
      Stats.Directories++;
      return 0;
    }
    Stats.Files++;
    return Info.size();
  }
  Stats.Directories++;
  qint64 Size = 0;
  QDirIterator It(Path, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
  while (It.hasNext() && !Progress.Cancelled())
  {
    It.next();
    QFileInfo Child = It.fileInfo();
    if (Child.isSymLink())
    {
      Stats.SymLinks++;
    }
    if (Child.isDir())
    {
      Stats.Directories++;
    }
    else
    {
      Stats.Files++;
      Size += Child.size();
    }
  }
  return Size;
}

}

namespace FileOperations
{

QStringList LocalChecksumAlgs()
{
  // QCryptographicHash, WinSCP names
  return Terminal::ShellChecksumAlgs();
}

bool RemoteDirectoryExists(Terminal * Term, const QString & APath)
{
  // TCustomScpExplorerForm::DoDirectoryExists
  if ((Term == nullptr) || !Term->Active())
  {
    return false;
  }
  QString Path = UnixExcludeTrailingSlash(AbsoluteRemotePath(Term->CurrentDirectory(), APath));
  auto Exists = std::make_shared<bool>(false);
  WaitFor(nullptr, Term, [Term, Path, Exists](Terminal::Callback Done)
  {
    Term->RunOnWorker([Path, Exists](FileSystem & Fs, FileOperationProgress &, UserInterface &)
    {
      RemoteFile File;
      *Exists = Fs.FileExists(Path, &File) && File.IsDirectory;
    }, Done);
  }, false);
  return *Exists;
}

bool CalculateSize(QWidget * Parent, Terminal * Term, const QStringList & Paths, qint64 & Size,
  Terminal::CalculateSizeStats & Stats, QList<qint64> * Sizes)
{
  // TCustomScpExplorerForm::CalculateSize
  Size = 0;
  Stats = Terminal::CalculateSizeStats();
  if (Term == nullptr)
  {
    FileOperationProgress Progress;
    QList<qint64> Result;
    RunLocal(Parent, Progress, Paths.size(), [&]()
    {
      for (const QString & Path : Paths)
      {
        if (Progress.Cancelled())
        {
          break;
        }
        Progress.SetFile(Path, 0);
        qint64 ItemSize = LocalSize(Path, Stats, Progress);
        Result << ItemSize;
        Size += ItemSize;
        Progress.FileFinished();
      }
    });
    if (Sizes != nullptr)
    {
      *Sizes = Result;
    }
    return !Progress.Cancelled();
  }
  auto Calculated = std::make_shared<std::tuple<qint64, Terminal::CalculateSizeStats, QList<qint64>>>();
  OperationResult Result = WaitFor(Parent, Term, [Term, Paths, Calculated](Terminal::Callback Done)
  {
    Term->CalculateFilesSize(Paths, [Calculated, Done](const OperationResult & R, qint64 ASize,
      const Terminal::CalculateSizeStats & AStats, const QList<qint64> & ASizes)
    {
      *Calculated = std::make_tuple(ASize, AStats, ASizes);
      Done(R);
    });
  }, true);
  if (!Result.Ok)
  {
    if (!Result.Aborted && !Result.Fatal)
    {
      ShowErrorMessage(Parent, Result.Message, Result.MoreMessages);
    }
    return false;
  }
  Size = std::get<0>(*Calculated);
  Stats = std::get<1>(*Calculated);
  if (Sizes != nullptr)
  {
    *Sizes = std::get<2>(*Calculated);
  }
  return true;
}

void CalculateChecksum(QWidget * Parent, Terminal * Term, const QString & Alg, const QStringList & Paths,
  const Terminal::ChecksumCallback & OnChecksum)
{
  // TCustomScpExplorerForm::CalculateChecksum
  if (Term == nullptr)
  {
    FileOperationProgress Progress;
    struct Entry
    {
      QString Name;
      QString Checksum;
    };
    QList<Entry> Results;
    QString Error;
    QString NormalizedAlg = Alg;
    for (const QString & Known : LocalChecksumAlgs())
    {
      if (QString(Known).remove(QLatin1Char('-')).compare(QString(Alg).remove(QLatin1Char('-')), Qt::CaseInsensitive) == 0)
      {
        NormalizedAlg = Known;
      }
    }
    RunLocal(Parent, Progress, Paths.size(), [&]()
    {
      try
      {
        for (const QString & Path : Paths)
        {
          QFileInfo Info(Path);
          if (Info.isDir())
          {
            if (Info.isSymLink())
            {
              continue;
            }
            QDirIterator It(Path, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
            while (It.hasNext() && !Progress.Cancelled())
            {
              QString File = It.next();
              Progress.SetFile(File, 0);
              Results.append({Info.fileName() + QLatin1Char('/') + QDir(Path).relativeFilePath(File), LocalFileChecksum(File, Alg)});
            }
          }
          else
          {
            Progress.SetFile(Path, 0);
            Results.append({Info.fileName(), LocalFileChecksum(Path, Alg)});
          }
          if (Progress.Cancelled())
          {
            break;
          }
          Progress.FileFinished();
        }
      }
      catch (const ExtException & E)
      {
        Error = E.Message() + QLatin1Char('\n') + E.MoreMessages();
      }
    });
    for (const Entry & E : Results)
    {
      OnChecksum(E.Name, NormalizedAlg, E.Checksum);
    }
    if (!Error.isEmpty())
    {
      ShowErrorMessage(Parent, Error.section(QLatin1Char('\n'), 0, 0), Error.section(QLatin1Char('\n'), 1));
    }
    return;
  }
  OperationResult Result = WaitFor(Parent, Term, [Term, Alg, Paths, OnChecksum](Terminal::Callback Done)
  {
    Term->CalculateFilesChecksum(Alg, Paths, OnChecksum, Done);
  }, true);
  if (!Result.Ok && !Result.Aborted && !Result.Fatal)
  {
    ShowErrorMessage(Parent, Result.Message, Result.MoreMessages);
  }
}

void Properties(QWidget * Parent, Terminal * Term, const QList<RemoteFile> & Files, const QStringList & Paths,
  const QString & Directory, const Runner & Run, std::function<void(bool Changed)> Done)
{
  // TCustomScpExplorerForm::RemoteDirViewProperties / SetProperties
  if (Files.isEmpty())
  {
    return;
  }
  bool Remote = (Term != nullptr);
  QPointer<Terminal> GuardTerm(Term);
  int AllowedChanges = 0;
  QStringList Users, Groups;
  for (const RemoteFile & File : Files)
  {
    AddUnique(Users, File.Owner);
    AddUnique(Groups, File.Group);
  }
  if (Remote)
  {
    AllowedChanges =
      (Term->IsCapable(fcModeChanging) ? PropertiesDialog::cpMode : 0) |
      (Term->IsCapable(fcOwnerChanging) ? PropertiesDialog::cpOwner : 0) |
      (Term->IsCapable(fcGroupChanging) ? PropertiesDialog::cpGroup : 0);
    if ((AllowedChanges & (PropertiesDialog::cpOwner | PropertiesDialog::cpGroup)) && Term->IsCapable(fcUserGroupListing))
    {
      // TTerminal::LookupUsersGroups (once per session in WinSCP; cheap here)
      auto LookedUp = std::make_shared<std::pair<QStringList, QStringList>>();
      WaitFor(Parent, Term, [Term, LookedUp](Terminal::Callback Callback)
      {
        Term->LookupUsersGroups([LookedUp, Callback](const OperationResult & R, const QStringList & U, const QStringList & G)
        {
          *LookedUp = {U, G};
          Callback(R);
        });
      }, false);
      for (const QString & U : LookedUp->first) AddUnique(Users, U);
      for (const QString & G : LookedUp->second) AddUnique(Groups, G);
    }
    else if (AllowedChanges & (PropertiesDialog::cpOwner | PropertiesDialog::cpGroup))
    {
      // SFTP: the names seen in the listings
      auto LookedUp = std::make_shared<std::pair<QStringList, QStringList>>();
      WaitFor(Parent, Term, [Term, LookedUp](Terminal::Callback Callback)
      {
        Term->RunOnWorker([LookedUp](FileSystem & Fs, FileOperationProgress &, UserInterface &)
        {
          Fs.LookupUsersGroups(LookedUp->first, LookedUp->second);
        }, Callback);
      }, false);
      for (const QString & U : LookedUp->first) AddUnique(Users, U);
      for (const QString & G : LookedUp->second) AddUnique(Groups, G);
    }
    if (!GuardTerm)
    {
      return;
    }
  }
  else
  {
    // Linux: chmod/chown of local files
    AllowedChanges = PropertiesDialog::cpMode | PropertiesDialog::cpOwner | PropertiesDialog::cpGroup;
    LocalUsersGroups(Users, Groups);
  }
  Users.sort(Qt::CaseInsensitive);
  Groups.sort(Qt::CaseInsensitive);

  PropertiesDialog Dialog(Files, Directory, Remote, Parent);
  Dialog.SetAllowedChanges(AllowedChanges);
  Dialog.SetUsersGroups(Users, Groups);
  Dialog.SetOnCalculateSize([&Dialog, GuardTerm, Remote, Paths](qint64 & Size, PropertiesDialog::SizeStats & Stats)
  {
    if (Remote && !GuardTerm)
    {
      return false;
    }
    Terminal::CalculateSizeStats Calculated;
    bool Result = CalculateSize(&Dialog, Remote ? GuardTerm.data() : nullptr, Paths, Size, Calculated);
    Stats.Files = Calculated.Files;
    Stats.Directories = Calculated.Directories;
    Stats.SymLinks = Calculated.SymLinks;
    return Result;
  });
  QStringList Algs = Remote ? Term->SupportedChecksumAlgs() : LocalChecksumAlgs();
  if (!Algs.isEmpty())
  {
    Dialog.SetChecksum(Algs, [&Dialog, GuardTerm, Remote, Paths](const QString & Alg, const PropertiesDialog::ChecksumResultFunc & OnChecksum)
    {
      if (Remote && !GuardTerm)
      {
        return;
      }
      CalculateChecksum(&Dialog, Remote ? GuardTerm.data() : nullptr, Alg, Paths, OnChecksum);
    });
  }
  if ((Dialog.exec() != QDialog::Accepted) || (Remote && !GuardTerm))
  {
    if (Done) Done(false);
    return;
  }
  RemoteProperties Changed = Dialog.Properties();
  if (Changed.Empty() && !Changed.Recursive)
  {
    if (Done) Done(false);
    return;
  }
  if (Remote)
  {
    RunOperation(Parent, Term, Run, [Term, Paths, Changed](Terminal::Callback Callback)
    {
      Term->ChangeFilesProperties(Paths, Changed, Callback);
    },
    [Done](const OperationResult & Result)
    {
      Q_UNUSED(Result);
      if (Done) Done(true);
    });
  }
  else
  {
    QStringList Errors = ChangeLocalProperties(Paths, Changed);
    if (!Errors.isEmpty())
    {
      ShowErrorMessage(Parent, Errors.first().section(QLatin1Char('\n'), 0, 0),
        Errors.join(QStringLiteral("\n\n")));
    }
    if (Done) Done(true);
  }
}

void CreateDirectory(QWidget * Parent, Terminal * Term, const QString & Directory, const Runner & Run,
  std::function<void(const QString & Path)> Done)
{
  // TCustomScpExplorerForm::CreateDirectory
  bool Remote = (Term != nullptr);
  RemoteProperties Properties = CreateDirectoryDialog::LoadNewDirectoryProperties();
  CreateDirectoryDialog Dialog(Remote, Parent);
  // NEW_FOLDER
  Dialog.SetDirectoryName(QStringLiteral("New folder"));
  Dialog.SetAllowedChanges((Remote && Term->IsCapable(fcModeChanging)) ? PropertiesDialog::cpMode : 0);
  Dialog.SetProperties(Properties);
  Dialog.SetSaveSettings(false);
  QPointer<Terminal> GuardTerm(Term);
  if ((Dialog.exec() != QDialog::Accepted) || (Remote && !GuardTerm))
  {
    return;
  }
  QString Name = Dialog.DirectoryName();
  if (Remote)
  {
    RemoteProperties NewProperties = Dialog.Properties();
    if (Dialog.SaveSettings())
    {
      CreateDirectoryDialog::SaveNewDirectoryProperties(NewProperties);
    }
    QString Path = AbsoluteRemotePath(Directory, Name);
    RunOperation(Parent, Term, Run, [Term, Path, NewProperties](Terminal::Callback Callback)
    {
      Term->CreateDirectory(Path, NewProperties, Callback);
    },
    [Done, Path](const OperationResult & Result)
    {
      if (Result.Ok && Done) Done(Path);
    });
  }
  else
  {
    QString Path = QDir(Directory).absoluteFilePath(Name);
    if (QFileInfo::exists(Path) || !QDir().mkpath(Path))
    {
      // CREATE_DIR_ERROR
      ShowErrorMessage(Parent, QStringLiteral("Error creating folder '%1'.").arg(Name));
      return;
    }
    if (Done) Done(Path);
  }
}

void CalculateDirectorySizes(QWidget * Parent, Terminal * Term, const QStringList & Paths,
  std::function<void(const QHash<QString, qint64> & Sizes)> Done)
{
  // TCustomScpExplorerForm::CalculateDirectorySizes
  qint64 Size = 0;
  Terminal::CalculateSizeStats Stats;
  QList<qint64> Sizes;
  QHash<QString, qint64> Result;
  if (CalculateSize(Parent, Term, Paths, Size, Stats, &Sizes))
  {
    for (qsizetype Index = 0; (Index < Paths.size()) && (Index < Sizes.size()); Index++)
    {
      if (Sizes[Index] >= 0)
      {
        Result.insert(Paths[Index], Sizes[Index]);
      }
    }
  }
  if (Done)
  {
    Done(Result);
  }
}

void AddEditLink(QWidget * Parent, Terminal * Term, const QString & Directory, const RemoteFile * Focused,
  std::function<void(bool Created)> Done)
{
  // TCustomScpExplorerForm::AddEditLink
  bool Remote = (Term != nullptr);
  bool Edit = (Focused != nullptr) && Focused->IsSymLink && !Focused->IsParentDirectory;
  QString FileName, PointTo;
  if (Edit)
  {
    FileName = Focused->FileName;
    PointTo = Focused->LinkTo;
    if (!Remote && PointTo.isEmpty())
    {
      PointTo = QFile::symLinkTarget(QDir(Directory).filePath(FileName));
    }
  }
  else if ((Focused != nullptr) && !Focused->IsParentDirectory)
  {
    PointTo = Focused->FileName;
  }
  bool SymbolicLink = true;
  // Linux: hard links are possible locally too
  bool AllowHardLink = Remote ? Term->IsCapable(fcHardLink) : true;
  QPointer<Terminal> GuardTerm(Term);
  LinkDialog Dialog(Parent);
  if (!Dialog.Execute(FileName, PointTo, SymbolicLink, Edit, AllowHardLink) || (Remote && !GuardTerm))
  {
    return;
  }
  if (Remote)
  {
    QString Path = AbsoluteRemotePath(Directory, FileName);
    auto Report = [Done](const OperationResult & Result) { if (Result.Ok && Done) Done(true); };
    if (Edit)
    {
      // an existing link is deleted and created again
      RunOperation(Parent, Term, Runner(), [Term, Path, PointTo](Terminal::Callback Callback)
      {
        Term->RunOnWorker([Path, PointTo](FileSystem & Fs, FileOperationProgress &, UserInterface &)
        {
          Fs.DeleteFile(Path);
          Fs.CreateLink(Path, PointTo);
        }, Callback);
      }, Report);
    }
    else
    {
      RunOperation(Parent, Term, Runner(), [Term, Path, PointTo, SymbolicLink](Terminal::Callback Callback)
      {
        Term->CreateLink(Path, PointTo, SymbolicLink, Callback);
      }, Report);
    }
    return;
  }
  // Linux: symlink(2) / link(2) instead of a Windows .lnk shortcut
  QString Path = QDir(Directory).absoluteFilePath(FileName);
  QByteArray Name = QFile::encodeName(Path);
  QByteArray Target = QFile::encodeName(PointTo);
  if (Edit && (unlink(Name.constData()) != 0))
  {
    ShowErrorMessage(Parent, QStringLiteral("Cannot create link '%1'.").arg(Path), ErrnoText());
    return;
  }
  int Rc;
  if (SymbolicLink)
  {
    Rc = symlink(Target.constData(), Name.constData());
  }
  else
  {
    // a relative target is relative to the link's directory, as with "ln"
    QByteArray AbsoluteTarget = QFile::encodeName(QDir(QFileInfo(Path).absolutePath()).absoluteFilePath(PointTo));
    Rc = link(AbsoluteTarget.constData(), Name.constData());
  }
  if (Rc != 0)
  {
    // CREATE_LINK_ERROR
    ShowErrorMessage(Parent, QStringLiteral("Cannot create link '%1'.").arg(Path), ErrnoText());
    return;
  }
  if (Done) Done(true);
}

void Duplicate(QWidget * Parent, Terminal * Term, const QList<RemoteFile> & Files, const QStringList & Paths,
  const QString & Target, const QList<RemoteTransferDialog::SessionEntry> & ASessions, const Runner & Run,
  std::function<void(Terminal * TargetSession)> Done, bool TargetConfirmed)
{
  // TCustomScpExplorerForm::RemoteTransferDialog + RemoteTransferFiles (not Move)
  if (Paths.isEmpty() || (Term == nullptr))
  {
    return;
  }
  bool AnyDirectory = false;
  for (const RemoteFile & File : Files)
  {
    AnyDirectory = AnyDirectory || File.IsDirectory;
  }
  Terminal::DirectRemoteCopy AllowDirectCopy = Term->AllowDirectRemoteCopy(AnyDirectory);
  bool DirectCopy = (AllowDirectCopy != Terminal::drcDisallow);
  QString ATarget = Target;
  QString FileMask = (Paths.size() == 1) ? DelimitFileNameMask(UnixPath::ExtractFileName(Paths.first())) : AnyMask;
  QList<RemoteTransferDialog::SessionEntry> Sessions = ASessions;
  bool HasCurrent = false;
  for (const RemoteTransferDialog::SessionEntry & Entry : Sessions)
  {
    HasCurrent = HasCurrent || (Entry.Session == Term);
  }
  if (!HasCurrent)
  {
    Sessions.prepend({Term->Data().SessionName(), Target, Term});
  }
  QPointer<Terminal> GuardTerm(Term);
  void * Session = Term;
  RemoteTransferDialog Dialog(Parent);
  Dialog.Init(Paths.size() > 1, Sessions, AllowDirectCopy, Term,
    [GuardTerm](void * ASession, const QString & Path)
    {
      Terminal * T = (ASession != nullptr) ? static_cast<Terminal *>(ASession) : GuardTerm.data();
      return RemoteDirectoryExists(T, Path);
    }, TargetConfirmed);
  if (!Dialog.Execute(Session, ATarget, FileMask, DirectCopy) || !GuardTerm)
  {
    return;
  }
  auto * TargetTerm = static_cast<Terminal *>(Session);
  QString TargetDir = UnixExcludeTrailingSlash(AbsoluteRemotePath(Target, ATarget));
  if ((TargetTerm == Term) && DirectCopy)
  {
    RunOperation(Parent, Term, Run, [Term, Paths, TargetDir, FileMask](Terminal::Callback Callback)
    {
      Term->CopyFiles(Paths, TargetDir, FileMask, Callback);
    },
    [Done, Term](const OperationResult &) { if (Done) Done(Term); });
    return;
  }
  // Duplicate via local temporary copy (also to another session):
  // TemporarilyDownloadFiles + CopyToRemote
  auto TempDir = std::make_shared<QTemporaryDir>();
  if (!TempDir->isValid())
  {
    ShowErrorMessage(Parent, QStringLiteral("Cannot create temporary directory."), TempDir->errorString());
    return;
  }
  CopyParam Param = Configuration::Instance().DefaultCopyParam();
  Param.FileMask = AnyMask;
  QPointer<Terminal> GuardTarget(TargetTerm);
  QPointer<QWidget> GuardParent(Parent);
  RunOperation(Parent, Term, Run, [Term, Paths, TempDir, Param](Terminal::Callback Callback)
  {
    Term->CopyToLocal(Paths, TempDir->path(), Param, false, Callback);
  },
  [GuardParent, GuardTarget, Term, Run, Paths, TempDir, Param, TargetDir, FileMask, Done](const OperationResult & Result)
  {
    if (!Result.Ok || !GuardTarget)
    {
      return;
    }
    // the target name mask, applied to the temporary copies
    QStringList LocalPaths;
    for (const QString & Path : Paths)
    {
      QString Name = UnixPath::ExtractFileName(Path);
      QString Masked = MaskFileName(Name, FileMask);
      QString Local = QDir(TempDir->path()).filePath(Name);
      if (!QFileInfo::exists(Local) && !QFileInfo(Local).isSymLink())
      {
        continue;
      }
      if (Masked != Name)
      {
        QString Renamed = QDir(TempDir->path()).filePath(Masked);
        if (QFile::rename(Local, Renamed))
        {
          Local = Renamed;
        }
      }
      LocalPaths << Local;
    }
    if (LocalPaths.isEmpty())
    {
      return;
    }
    Terminal * TargetTerm = GuardTarget.data();
    RunOperation(GuardParent, TargetTerm, (TargetTerm == Term) ? Run : Runner(),
      [TargetTerm, LocalPaths, TargetDir, Param](Terminal::Callback Callback)
      {
        TargetTerm->CopyToRemote(LocalPaths, TargetDir, Param, false, Callback);
      },
      [TempDir, Done, GuardTarget](const OperationResult &)
      {
        if (Done && GuardTarget) Done(GuardTarget.data());
      });
  });
}

void MoveTo(QWidget * Parent, Terminal * Term, const QStringList & Paths, const QString & Target, const Runner & Run,
  std::function<void(bool Moved)> Done)
{
  // TCustomScpExplorerForm::RemoteTransferDialog + RemoteTransferFiles (Move)
  if (Paths.isEmpty() || (Term == nullptr))
  {
    return;
  }
  QString ATarget = Target;
  QString FileMask = (Paths.size() == 1) ? DelimitFileNameMask(UnixPath::ExtractFileName(Paths.first())) : AnyMask;
  QPointer<Terminal> GuardTerm(Term);
  RemoteMoveDialog Dialog(Paths.size() > 1, [GuardTerm](void *, const QString & Path)
  {
    return RemoteDirectoryExists(GuardTerm.data(), Path);
  }, Parent);
  if (!Dialog.Execute(ATarget, FileMask) || !GuardTerm)
  {
    return;
  }
  QString TargetDir = UnixExcludeTrailingSlash(AbsoluteRemotePath(Target, ATarget));
  RunOperation(Parent, Term, Run, [Term, Paths, TargetDir, FileMask](Terminal::Callback Callback)
  {
    Term->MoveFiles(Paths, TargetDir, FileMask, Callback);
  },
  [Done](const OperationResult & Result) { if (Done) Done(Result.Ok); });
}

ServerInfoDialog::GetSpaceAvailableFunc SpaceAvailableFunc(QWidget * Parent, Terminal * Term)
{
  if ((Term == nullptr) || !Term->IsCapable(fcCheckingSpaceAvailable))
  {
    return ServerInfoDialog::GetSpaceAvailableFunc();
  }
  QPointer<Terminal> GuardTerm(Term);
  QPointer<QWidget> GuardParent(Parent);
  return [GuardTerm, GuardParent](const QString & Path, ServerInfoDialog::SpaceAvailable & Result, QString & Error)
  {
    if (!GuardTerm)
    {
      return false;
    }
    auto Space = std::make_shared<SpaceAvailableInfo>();
    Terminal * T = GuardTerm.data();
    OperationResult R = WaitFor(GuardParent, T, [T, Path, Space](Terminal::Callback Callback)
    {
      T->SpaceAvailable(Path, [Space, Callback](const OperationResult & AResult, const SpaceAvailableInfo & S)
      {
        *Space = S;
        Callback(AResult);
      });
    }, false);
    if (!R.Ok)
    {
      Error = R.MoreMessages.isEmpty() ? R.Message : (R.Message + QLatin1Char('\n') + R.MoreMessages);
      return false;
    }
    Result.BytesOnDevice = Space->BytesOnDevice;
    Result.UnusedBytesOnDevice = Space->UnusedBytesOnDevice;
    Result.BytesAvailableToUser = Space->BytesAvailableToUser;
    Result.UnusedBytesAvailableToUser = Space->UnusedBytesAvailableToUser;
    Result.BytesPerAllocationUnit = Space->BytesPerAllocationUnit;
    return true;
  };
}

}
