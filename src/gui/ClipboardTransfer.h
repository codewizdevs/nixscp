// Files on the clipboard and in drags to other applications. Linux
// replacement of WinSCP's shell integration:
// - forms/CustomScpExplorer.cpp CopyFilesToClipboard / PasteFromClipBoard /
//   ClipboardFakeCreated / ClipboardDownload (Ctrl+C / Ctrl+V, "Copy to
//   Clipboard" and "Paste from Clipboard"), packages/dragndrop (CF_HDROP,
//   Preferred DropEffect)
// - forms/CustomScpExplorer.cpp RemoteFileControlDDCreateDataObject /
//   DDDownload / DoWarnLackOfTempSpace (drag remote files out to Explorer:
//   the "compatible" temporary folder mode; the shell extension mode has no
//   Linux counterpart)
// The freedesktop formats are used: text/uri-list, x-special/gnome-copied-files
// (GNOME, Xfce, Cinnamon, MATE file managers) and application/x-kde-cutselection
// (KDE). Remote files are downloaded only when another application asks for
// the data (paste or drop), into a private temporary folder; NixSCP's own
// panels get the remote paths and transfer directly.
#pragma once

#include "core/Terminal.h"

#include <QMimeData>
#include <QPointer>
#include <QStringList>

#include <functional>

class QWidget;

namespace ClipboardTransfer
{

QString UriListMimeType();          // text/uri-list
QString GnomeCopiedFilesMimeType(); // x-special/gnome-copied-files
QString KdeCutSelectionMimeType();  // application/x-kde-cutselection
QString FilesMimeType();            // application/x-nixscp-files (as FilePanel's drags)

// ---- Local files (Copy to Clipboard in the local panel) ----
// text/uri-list, x-special/gnome-copied-files ("copy"/"cut"),
// application/x-kde-cutselection, text/plain (the paths) and NixSCP's own format
QMimeData * CreateLocalMimeData(const QStringList & Paths, bool Cut = false);
void CopyLocalFiles(const QStringList & Paths, bool Cut = false);

// ---- Remote files ----
// Data of remote files of a session: NixSCP's own format and the remote
// paths as text are immediate; the file formats download the files on the
// first request (with the Copy dialog when DD confirmation is on, the
// lack of temporary space warning and the progress window) and offer the
// downloaded copies. A failed or cancelled download offers nothing.
class RemoteFilesMimeData : public QMimeData
{
  Q_OBJECT

public:
  // DragDrop: data of a drag (DDTransferConfirmation applies), else clipboard
  // (the Copy dialog is shown as for Paste). TotalSize: size of the files,
  // -1 when unknown (directories) - used by the temporary space check.
  RemoteFilesMimeData(Terminal * Term, const QStringList & RemotePaths, QWidget * DialogParent, bool DragDrop,
    qint64 TotalSize = -1);
  ~RemoteFilesMimeData() override;

  Terminal * SourceTerminal() const { return FTerminal; }
  QStringList RemotePaths() const { return FPaths; }
  bool IsDragDrop() const { return FDragDrop; }
  // Downloads the files (once) and returns the local copies; empty when the
  // download failed or was cancelled. Blocks with a local event loop.
  QStringList DownloadedFiles() const;
  bool Downloaded() const { return FAttempted && !FLocalFiles.isEmpty(); }
  // Folder the files were downloaded to (empty before)
  QString TemporaryDirectory() const { return FTemporaryDirectory; }

  QStringList formats() const override;
  bool hasFormat(const QString & MimeType) const override;

  // Test hook: replaces the transfer (Terminal::CopyToLocal + progress window)
  using Downloader = std::function<bool(Terminal * Term, const QStringList & RemotePaths, const QString & TargetDirectory)>;
  static void SetDownloader(const Downloader & ADownloader);

protected:
  QVariant retrieveData(const QString & MimeType, QMetaType Type) const override;

private:
  QPointer<Terminal> FTerminal;
  QStringList FPaths;
  QPointer<QWidget> FParent;
  bool FDragDrop;
  qint64 FTotalSize;
  mutable bool FAttempted = false;
  mutable bool FDownloading = false;
  mutable QStringList FLocalFiles;
  mutable QString FTemporaryDirectory;

  bool Download() const;
};

RemoteFilesMimeData * CreateRemoteMimeData(Terminal * Term, const QStringList & RemotePaths, QWidget * DialogParent,
  bool DragDrop, qint64 TotalSize = -1);
// Copy to Clipboard (Ctrl+C) in the remote panel
void CopyRemoteFiles(Terminal * Term, const QStringList & RemotePaths, QWidget * DialogParent, qint64 TotalSize = -1);

// ---- Paste ----
struct Content
{
  enum Kind { ckNone, ckLocalFiles, ckRemoteFiles, ckText };
  Kind Type = ckNone;
  QStringList Paths;          // local paths (ckLocalFiles) or remote paths (ckRemoteFiles)
  bool Cut = false;           // the files were cut in the file manager (move on paste)
  QPointer<Terminal> Source;  // session of remote files (null when it was closed)
  QPointer<RemoteFilesMimeData> Remote;  // remote files data (DownloadedFiles())
  QString Text;               // ckText: trimmed text
};
// Decodes Mime (default: the clipboard)
Content Read(const QMimeData * Mime = nullptr);
// CanPasteFromClipBoard: files, or a single line of text
bool CanPaste(const QMimeData * Mime = nullptr);

// What a pasted text means (TCustomScpExplorerForm::PasteFromClipBoard /
// CanPasteToDirViewFromClipBoard): a session URL to open (sftp://...), a
// directory to change to (absolute path of the panel's kind), or nothing
enum TextKind { tkNone, tkSessionUrl, tkPath };
TextKind ClassifyText(const QString & Text, bool RemotePanel, QString * Value = nullptr);

// ---- Duplicate (Ctrl+drag within the remote panel, paste into the same session) ----
// TTerminal::CopyFiles: FTP SITE CPFR/CPTO, S3 server side copy, otherwise
// "cp -p -r -f" in the session shell (SCP, or the secondary shell of SFTP).
// Existing targets are confirmed (FILE_OVERWRITE); DuplicateIfSame creates
// "name (copy)" when the target is the source itself.
bool CanDuplicateRemote(Terminal * Term);
void DuplicateRemoteFiles(Terminal * Term, const QStringList & Paths, const QString & TargetDirectory,
  Terminal::Callback Done, bool DuplicateIfSame = false);

} // namespace ClipboardTransfer
