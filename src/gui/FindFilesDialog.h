// Port of WinSCP source/forms/FileFind.{cpp,dfm} (Find Files window).
// The search itself is core/FileFind (remote via Terminal worker, or local).
//
// Non-modal top level window, deleted on close (Qt::WA_DeleteOnClose), as
// WinSCP's caFree. The operations on the results are delegated to the
// owner through signals; buttons whose signal is not connected are hidden.
#pragma once

#include <QDialog>
#include <QStringList>

class Terminal;

class FindFilesDialog : public QDialog
{
  Q_OBJECT

public:
  // Term == nullptr: search the local file system
  FindFilesDialog(Terminal * Term, const QString & StartDirectory, QWidget * Parent = nullptr);
  ~FindFilesDialog() override;

  // TFileFindDialog::Init: reuse an open window for another session/directory
  // (results are cleared only when the session changes)
  void Init(Terminal * Term, const QString & StartDirectory);

  // Removes the results of successfully deleted files (after DeleteRequested)
  void FilesDeleted(const QStringList & Paths);

signals:
  // "Focus": show the file in the panel (Directory is the containing directory)
  void FocusRequested(const QString & Directory, const QString & FileName);
  // Operations on the selected results (full paths)
  void EditRequested(const QStringList & Paths);
  void DownloadRequested(const QStringList & Paths);
  void DeleteRequested(const QStringList & Paths);

protected:
  void closeEvent(QCloseEvent * Event) override;
  void keyPressEvent(QKeyEvent * Event) override;
  void showEvent(QShowEvent * Event) override;
  void reject() override;

private:
  struct Private;
  Private * d;
};
