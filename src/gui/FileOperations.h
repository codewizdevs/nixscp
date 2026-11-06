// The file operations behind the Properties dialog and the Files menu, as
// WinSCP's source/forms/CustomScpExplorer.cpp runs them
// (TCustomScpExplorerForm::RemoteDirViewProperties / SetProperties,
// CalculateSize, CalculateChecksum, CreateDirectory, RemoteTransferFiles
// (Duplicate / Move To), AddEditLink, CalculateDirectorySizes, and
// TFileSystemInfoDialog's space available callback).
//
// Each entry point shows the dialog, runs the operation (remote ones on the
// session's worker thread with a progress window) and reports completion
// on the GUI thread. Term == nullptr means the local panel (Linux: chmod,
// chown, symlink/link, QCryptographicHash).
#pragma once

#include "core/Terminal.h"
#include "gui/RemoteTransferDialog.h"
#include "gui/ServerInfoDialog.h"

#include <QHash>
#include <QString>
#include <QStringList>

#include <functional>

class QWidget;

namespace FileOperations
{
  // In-place rename: the character making Name impossible, empty when it is
  // fine. WindowsPath: a local name on Windows, where "\" separates directories too.
  QString InvalidNameCharacter(const QString & Name, bool WindowsPath);
  // Local rename of OldName to NewName, TargetExists: whether the target path
  // exists. On a case-insensitive volume the target of "foo" to "Foo" is the
  // file itself, which only DirectoryEntries (called then) tells apart from
  // another file: there the entry under the new name is missing.
  bool LocalRenameTargetTaken(const QString & OldName, const QString & NewName, bool TargetExists,
    const std::function<QStringList()> & DirectoryEntries);

  // Runs a Terminal operation with a progress window and reports its error
  // (CommanderWindow::RunWithProgress has this signature). Empty: a
  // ProgressDialog of the operation's terminal and ShowErrorMessage.
  using Runner = std::function<void(const std::function<void(Terminal::Callback)> & Start, const Terminal::Callback & Done)>;

  // F9 / Alt+Enter: Properties (Owner/Group combos when the protocol can
  // change them, tri-state permissions for multiple files, Calculate,
  // Checksum tab); applies only what was changed. Changed(true) after the
  // change was applied, Changed(false) when cancelled or nothing changed.
  void Properties(QWidget * Parent, Terminal * Term, const QList<RemoteFile> & Files, const QStringList & Paths,
    const QString & Directory, const Runner & Run, std::function<void(bool Changed)> Done = nullptr);

  // F7: Create Directory with the permissions frame and "Use same settings
  // next time" (NewDirectory2); Done gets the created path
  void CreateDirectory(QWidget * Parent, Terminal * Term, const QString & Directory, const Runner & Run,
    std::function<void(const QString & Path)> Done = nullptr);

  // Alt+Shift+Enter: Calculate Directory Sizes of the selected directories
  // (Paths); Done gets path -> size of each calculated directory, for the
  // panel's Size column (empty when cancelled)
  void CalculateDirectorySizes(QWidget * Parent, Terminal * Term, const QStringList & Paths,
    std::function<void(const QHash<QString, qint64> & Sizes)> Done);

  // Alt+F6: Add/Edit Link. Focused (may be null): a focused symbolic link is
  // edited (new target, same name), otherwise a new link is added with the
  // focused name prefilled as target. Done(true) when a link was created.
  void AddEditLink(QWidget * Parent, Terminal * Term, const QString & Directory, const RemoteFile * Focused,
    std::function<void(bool Created)> Done = nullptr);

  // Shift+F5: Duplicate. Sessions lists the open sessions for "Target
  // session" (Session = their Terminal *, the current one included); a copy
  // to another session or "via local temporary copy" downloads to a
  // temporary directory and uploads from there. TargetConfirmed: Target is
  // a drop target directory (Ctrl+drag), not just the current directory.
  // Done gets the session the files were copied to.
  void Duplicate(QWidget * Parent, Terminal * Term, const QList<RemoteFile> & Files, const QStringList & Paths,
    const QString & Target, const QList<RemoteTransferDialog::SessionEntry> & Sessions, const Runner & Run,
    std::function<void(Terminal * TargetSession)> Done = nullptr, bool TargetConfirmed = false);

  // Shift+F6: Move To (target path + mask, RemoteTarget history)
  void MoveTo(QWidget * Parent, Terminal * Term, const QStringList & Paths, const QString & Target, const Runner & Run,
    std::function<void(bool Moved)> Done = nullptr);

  // TFileSystemInfoDialog OnGetSpaceAvailable for ServerInfoDialog::Execute
  // (null when the session cannot check space available)
  ServerInfoDialog::GetSpaceAvailableFunc SpaceAvailableFunc(QWidget * Parent, Terminal * Term);

  // Blocking helpers (nested event loop; progress window for longer runs)
  bool RemoteDirectoryExists(Terminal * Term, const QString & Path);
  bool CalculateSize(QWidget * Parent, Terminal * Term, const QStringList & Paths, qint64 & Size,
    Terminal::CalculateSizeStats & Stats, QList<qint64> * Sizes = nullptr);
  void CalculateChecksum(QWidget * Parent, Terminal * Term, const QString & Alg, const QStringList & Paths,
    const Terminal::ChecksumCallback & OnChecksum);
  // Algorithms offered for local files (QCryptographicHash)
  QStringList LocalChecksumAlgs();
}
