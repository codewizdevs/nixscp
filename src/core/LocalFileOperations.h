// Local file operations: the local-to-local copy/move engine and local file
// properties. Replaces what WinSCP delegates to the Windows shell:
// - packages/filemng/DirView.pas TDirView.PerformDragDropFileOperation and
//   CustomDirView (drag & drop within/between local panels, SHFileOperation
//   copy/move with progress), forms/CustomScpExplorer.cpp LocalCopyFiles /
//   DoDirViewExecFile (local-local Copy/Move, F5/F6),
// - the local Properties dialog (Windows shell properties, chmod/chown here).
//
// The engine runs on any thread; all prompts go through UserInterface
// (blocking, as the Terminal operations), progress and cancel through
// FileOperationProgress (so gui/ProgressDialog can show it).
//
// Linux specifics (the reason this is not QFile::copy):
// - files are cloned with ioctl(FICLONE) (instant on btrfs/xfs/bcachefs),
//   otherwise copied in the kernel with copy_file_range(2), falling back to
//   read/write; cancel and progress are checked between chunks
// - the copy is written to a temporary file next to the target and renamed
//   over it only after it is complete, so an existing target is never lost
//   when the copy fails or is cancelled
// - symbolic links are copied as links, timestamps (atime/mtime) and
//   permissions are kept, the owner too when allowed
// - a move within one file system is rename(2); across file systems it is a
//   copy followed by deleting the source (only what was copied successfully)
#pragma once

#include <QString>
#include <QStringList>

class FileOperationProgress;
class UserInterface;

struct LocalOperationResult
{
  bool Ok = true;             // no error and not aborted (skipped files do not count as errors)
  bool Aborted = false;       // cancelled by the user (or Abort answer)
  QString Message;            // the error that ended the operation
  QString MoreMessages;
  int Processed = 0;          // top level items copied/moved
  int Skipped = 0;            // files/directories skipped (No / Skip answers)
  QStringList Targets;        // full target paths of the processed top level items
};

class LocalFileOperations
{
public:
  enum Operation { opCopy, opMove };

  struct Params
  {
    Operation Op = opCopy;
    QStringList Sources;            // absolute local paths
    QString TargetDirectory;        // existing local directory
    // Single source only: name of the copy (empty = the source name)
    QString NewName;
    // Copy onto itself (paste into the same directory): create "name (copy).ext"
    // instead of skipping the item
    bool DuplicateIfSame = false;
    bool ConfirmOverwrite = true;   // ask before replacing (else replace)
    bool PreserveTime = true;
    bool PreserveRights = true;
    bool PreserveOwner = true;      // best effort (only root or own group changes succeed)
    bool UseReflink = true;         // ioctl(FICLONE) when the file system supports it
  };

  // Runs the operation synchronously (call from a worker thread when UI
  // prompts through a GUI UserInterface). UI may be null: then existing
  // targets are overwritten only when ConfirmOverwrite is off (else skipped)
  // and errors abort the operation. Progress may be null.
  static LocalOperationResult Execute(const Params & AParams, FileOperationProgress * Progress, UserInterface * UI);

  // Moves a file or directory to the desktop trash (DeleteToRecycleBin):
  // QFile::moveToTrash (freedesktop Trash specification: the home trash, or
  // $topdir/.Trash(-)$uid on other volumes), else "gio trash" (GVfs, e.g.
  // network/FUSE mounts). False with Error when the volume has no trash; the
  // caller may then offer deleting permanently (as the Windows shell does
  // for drives without a Recycle Bin). Blocking; any thread.
  static bool MoveToTrash(const QString & Path, QString * Error = nullptr);

  // Name of a duplicate in the same directory: "a.txt" -> "a (copy).txt",
  // then "a (copy 2).txt"... (first one that does not exist in Directory)
  static QString DuplicateName(const QString & Directory, const QString & FileName);

  // Whether Path (or one of its parents) is Directory, i.e. copying a
  // directory into its own subtree (both are made absolute and clean)
  static bool IsSubPath(const QString & Directory, const QString & Path);

  // ---- Local Properties (chmod / chown / chgrp) ----
  struct PropertiesParams
  {
    QStringList Paths;
    bool ChangeMode = false;
    unsigned int Mode = 0;          // permission bits (07777)
    // Tri-state masks as WinSCP TRights: bits in SetMask are set, bits in
    // ClearMask cleared, others kept (per file); used when ModeMasks is true
    bool ModeMasks = false;
    unsigned int SetMask = 0;
    unsigned int ClearMask = 0;
    bool AddXToDirectories = false; // TRights::AddExecute on directories (x where r)
    QString Owner;                  // user name or numeric uid; empty = keep
    QString Group;                  // group name or numeric gid; empty = keep
    bool Recursive = false;         // also the contents of directories
  };
  static LocalOperationResult ChangeProperties(const PropertiesParams & AParams, FileOperationProgress * Progress,
    UserInterface * UI);

  // Owner/group choices for the Properties dialog (passwd / group databases)
  static QStringList UserNames();
  static QStringList GroupNames();
  static QString OwnerName(unsigned int Uid);
  static QString GroupName(unsigned int Gid);
};
