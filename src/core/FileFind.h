// Port of WinSCP source/core/Terminal.cpp TTerminal::FilesFind / DoFilesFind /
// FileFind (recursive search honouring TFileMasks incl. size/time conditions,
// symlink loop detection), extended with an equivalent local search.
//
// The search runs off the GUI thread: remote searches as a Terminal::RunOnWorker
// job, local searches on a private thread. Results are queued and delivered on
// the GUI thread in batches (FilesFound), so the caller never touches the
// worker. Usage:
//   FileFind * Find = new FileFind(this);
//   connect(Find, &FileFind::FilesFound, ...); connect(Find, &FileFind::Finished, ...);
//   QString Error;
//   if (!Find->Start(Term /*nullptr = local*/, Directory, Mask, &Error)) { ...invalid mask... }
//   Find->Stop();   // cancels (Finished is still emitted, with Aborted)
#pragma once

#include "core/RemoteFile.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <memory>

class Terminal;
class QTimer;

struct FoundFile
{
  RemoteFile File;
  QString Directory;     // directory containing the file (no trailing slash, "/" for root)
  QString FullPath;      // Directory + "/" + File.FileName
};

class FileFind : public QObject
{
  Q_OBJECT

public:
  explicit FileFind(QObject * Parent = nullptr);
  ~FileFind() override;

  // Term == nullptr searches the local file system. Mask uses the full
  // WinSCP file mask syntax; returns false (with ErrorMessage) when the mask
  // is invalid or a search is already running.
  bool Start(Terminal * Term, const QString & Directory, const QString & Mask, QString * ErrorMessage = nullptr);
  void Stop();
  bool Finding() const { return FFinding; }
  // Directories that could not be read during the last search ("path: error")
  QStringList Errors() const { return FErrors; }

  // Blocking search on the calling thread (local only; used by tests and scripting)
  static QList<FoundFile> FindLocal(const QString & Directory, const QString & Mask, QStringList * Errors = nullptr);

  struct State;   // shared with the search thread (implementation detail)

signals:
  void FilesFound(const QList<FoundFile> & Files);
  void SearchingDirectory(const QString & Directory);
  // Error: the start directory could not be read or the connection failed
  void Finished(bool Aborted, const QString & Error);

private:
  std::shared_ptr<State> FState;
  QPointer<Terminal> FTerminal;
  bool FRemote = false;
  bool FFinding = false;
  QTimer * FTimer;
  QString FLastDirectory;
  QStringList FErrors;

  void Poll();
  void Finish(bool Aborted, const QString & Error);
};
