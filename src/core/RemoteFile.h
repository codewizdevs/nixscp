// Port of WinSCP source/core/RemoteFiles.{h,cpp} (TRemoteFile, TRights subset)
#pragma once

#include <QString>
#include <QDateTime>
#include <QList>

class Rights
{
public:
  // Unix permission bits, as TRights
  static QString ToText(unsigned int Mode);       // "rwxr-xr-x"
  static QString ToOctal(unsigned int Mode);      // "0755"
  static bool FromOctal(const QString & Str, unsigned int & Mode);
};

// TModificationFmt (precision of a timestamp, e.g. of FTP listings)
enum TModificationFmt { mfNone, mfMDY, mfYMDHM, mfMDHM, mfFull };

struct RemoteFile
{
  enum Type { File, Directory, Symlink, Other };

  QString FileName;
  qint64 Size = 0;
  QDateTime Modification;
  TModificationFmt ModificationFmt = mfFull; // what Modification really holds (seconds of a LIST are made up)
  unsigned int Permissions = 0; // lower 12 bits, as in st_mode
  Type FileType = File;
  bool IsDirectory = false;     // also true for symlinks pointing to directories
  bool IsSymLink = false;
  bool IsParentDirectory = false;
  bool IsHidden = false;
  QString LinkTo;
  QString Owner;
  QString Group;

  QString RightsText() const;   // "drwxr-xr-x"-like, without type letter: "rwxr-xr-x"
  QString TypeChar() const;     // 'd', 'l', '-'
};

using RemoteFileList = QList<RemoteFile>;

// Read as a Windows path, Name would leave the folder it is stored in: "..\x",
// "\\server\share", "C:x", ".. ". Any other name with a backslash or a colon
// stays listed, and CopyParam::ValidLocalFileName encodes it. On any platform,
// for the tests.
bool LeavesWindowsFolder(const QString & Name);

// A name from a server listing that is safe to use as a single path
// component (no traversal through "..", "/" or absolute names)
inline bool IsValidListingName(const QString & N)
{
  return !N.isEmpty() && (N != QStringLiteral(".")) && (N != QStringLiteral("..")) &&
#ifdef Q_OS_WIN
    !LeavesWindowsFolder(N) &&
#endif
    !N.contains(QLatin1Char('/')) && !N.contains(QChar(0));
}

namespace UnixPath
{
  QString Combine(const QString & Dir, const QString & Name);
  QString ExtractFileName(const QString & Path);
  QString ExtractFileDir(const QString & Path);
  QString Parent(const QString & Path);
  bool IsRoot(const QString & Path);
  // Can the last component of Path name a file or directory created locally?
  // A hostile server can list "x/..", and a transfer of a selected entry
  // names its target after this component. The root has no name to misuse.
  bool HasValidFileName(const QString & Path);
}
