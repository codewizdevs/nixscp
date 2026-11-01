// Port of WinSCP source/forms/Editor.{cpp,dfm} (internal editor): toolbar with
// Save/Save All/Reload/clipboard/undo/Find/Replace/Find Next/Go to line/
// Encoding/Color/Preferences/Help, Windows-like Find and Replace dialogs,
// status bar (line, column, character, encoding, state). The file is local;
// for remote files the owner (gui/EditorManager) downloads it to a temporary
// file first, uploads it again on Saved() and downloads it again on Reload
// (SetReloadHandler).
//
// Preferences > Editors > Internal editor are applied (and re-applied when the
// configuration changes): font, text and background colour, word wrap, tab
// size, default encoding, large file warning (Configuration\Interface\Editor:
// FontName2, FontSize, FontStyle, FontColor, BackgroundColor, WordWrap,
// TabSize, Encoding, WarnOnEncodingFallback, WarnOrLargeFileSize, LargeFileSize).
//
// Encodings: UTF-8, system default, ISO-8859-1; UTF-8/UTF-16 BOMs are
// detected and kept. Line endings (LF, CRLF, CR) are detected and kept.
//
// The window deletes itself when closed (Qt::WA_DeleteOnClose).
// Usage:
//   auto * Editor = new EditorWindow(TempFile, RemotePath, this);
//   if (!Editor->IsLoaded()) { delete Editor; return; }   // error already shown
//   connect(Editor, &EditorWindow::Saved, this, ...upload...);
//   Editor->show();
#pragma once

#include <QColor>
#include <QList>
#include <QMainWindow>

#include <functional>

class EditorWindow : public QMainWindow
{
  Q_OBJECT

public:
  enum Encoding { encUTF8, encSystem, encLatin1, encUTF16LE, encUTF16BE };
  enum LineEnding { eolLF, eolCRLF, eolCR };

  // Windows code pages as stored by WinSCP (Editor\Encoding, InternalEditorEncoding)
  static constexpr int CodePageUTF8 = 65001;   // CP_UTF8
  static constexpr int CodePageSystem = 0;     // CP_ACP

  // DisplayName is shown in the caption ("<DisplayName> - Editor - NixSCP");
  // empty = LocalFileName. EncodingOverride is the site's InternalEditorEncoding
  // (CodePageUTF8 / CodePageSystem; -1 = Preferences' default encoding).
  // Loading errors are reported in a message box.
  EditorWindow(const QString & LocalFileName, const QString & DisplayName, QWidget * Parent = nullptr,
    int EncodingOverride = -1);
  ~EditorWindow() override;

  bool IsLoaded() const;
  QString LocalFileName() const;
  bool IsModified() const;
  Encoding FileEncoding() const;
  LineEnding FileLineEnding() const;
  // Saves when modified; returns false on error (reported) or when not modified
  bool Save();
  void SetShowLineNumbers(bool Show);
  // Read-only editor (no saving), e.g. for files that cannot be uploaded
  void SetReadOnly(bool ReadOnly);
  // TEditorForm::NewFile: a file that does not exist on the server yet
  // (status "New", Reload and Encoding disabled until the first save)
  void SetNewFile(bool NewFile);
  // Background colour of this editor (session colour; Color button);
  // invalid = Preferences' default background colour
  void SetBackgroundColor(const QColor & Color);
  QColor BackgroundColor() const;

  // TEditorForm::OnFileReload: called by Reload before the file is read
  // again (remote files: download it again); the handler calls Done(true)
  // to continue loading, Done(false) when it failed (error already shown)
  using ReloadHandler = std::function<void(std::function<void(bool Ok)> Done)>;
  void SetReloadHandler(ReloadHandler Handler);
  // Reload (Ctrl+R/F5), asking first when the file was modified
  void Reload();

  // Closes without asking (TEditorForm destroyed without FormCloseQuery):
  // unsaved changes are written to "<file>.bak" (BackupSave) first
  void ForceClose();
  // Re-reads the internal editor preferences (connected to Configuration::Changed)
  void ApplyConfiguration();

  // Open internal editors (Save All, application exit)
  static QList<EditorWindow *> Instances();
  // Save All (Ctrl+Shift+S): saves all modified editors
  static void SaveAll();

  // Encoding name for the status bar ("UTF-8", "ISO-8859-1", system codeset)
  static QString EncodingName(Encoding AEncoding);

signals:
  // The file was written to LocalFileName (Ctrl+S, or "Yes" on close)
  void Saved(const QString & LocalFileName);
  // The window is closing (the owner may delete the temporary file after Saved finished)
  void Closed(const QString & LocalFileName);

protected:
  void closeEvent(QCloseEvent * Event) override;
  bool eventFilter(QObject * Watched, QEvent * Event) override;

private:
  struct Private;
  Private * d;
};
