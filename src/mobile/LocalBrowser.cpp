#include "mobile/LocalBrowser.h"
#include "mobile/AndroidFiles.h"

#include <QDir>
#include <QFileInfo>
#include <QUrl>

namespace
{
QString RightsText(QFileDevice::Permissions P)
{
  QString R;
  R += (P & QFileDevice::ReadOwner) ? 'r' : '-';
  R += (P & QFileDevice::WriteOwner) ? 'w' : '-';
  R += (P & QFileDevice::ExeOwner) ? 'x' : '-';
  R += (P & QFileDevice::ReadGroup) ? 'r' : '-';
  R += (P & QFileDevice::WriteGroup) ? 'w' : '-';
  R += (P & QFileDevice::ExeGroup) ? 'x' : '-';
  R += (P & QFileDevice::ReadOther) ? 'r' : '-';
  R += (P & QFileDevice::WriteOther) ? 'w' : '-';
  R += (P & QFileDevice::ExeOther) ? 'x' : '-';
  return R;
}

bool ValidName(const QString & Name)
{
  return !Name.isEmpty() && !Name.contains(QLatin1Char('/')) && (Name != QStringLiteral(".")) &&
    (Name != QStringLiteral(".."));
}
}

LocalBrowser::LocalBrowser(QObject * Parent) : QObject(Parent)
{
  FRoot = AndroidFiles::StorageRoot();
  FAccessible = AndroidFiles::HasStorageAccess();
  // Download (shown before the access is granted, then listed)
  FDirectory = QDir(FRoot).filePath(QStringLiteral("Download"));
  if (FAccessible && !QDir(FDirectory).exists())
  {
    FDirectory = FRoot;
  }
  if (FAccessible)
  {
    Load(FDirectory);
  }
}

QString LocalBrowser::displayDirectory() const
{
  QString Relative = QDir(FRoot).relativeFilePath(FDirectory);
  return (Relative == QStringLiteral(".")) ? QString() : Relative;
}

bool LocalBrowser::atRoot() const
{
  return QDir(FDirectory) == QDir(FRoot);
}

bool LocalBrowser::Load(const QString & Directory)
{
  QDir Dir(Directory);
  if (!Dir.exists() || !QFileInfo(Directory).isReadable())
  {
    FLastError = QStringLiteral("Cannot open the folder %1.").arg(Directory);
    emit failed(FLastError);
    return false;
  }
  QList<FileListModel::Entry> Entries;
  const QFileInfoList Infos = Dir.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
  for (const QFileInfo & Info : Infos)
  {
    FileListModel::Entry E;
    E.Name = Info.fileName();
    E.IsDir = Info.isDir();
    E.IsLink = Info.isSymLink();
    E.Size = Info.isDir() ? 0 : Info.size();
    E.Modified = Info.lastModified();
    E.Rights = RightsText(Info.permissions());
    if (!E.IsDir && (FileListModel::previewKind(E.Name) == QStringLiteral("image")))
    {
      E.Thumbnail = QUrl::fromLocalFile(Info.filePath()).toString();
    }
    Entries.append(E);
  }
  bool Changed = (QDir::cleanPath(Directory) != FDirectory);
  FDirectory = QDir::cleanPath(Directory);
  FFiles.SetEntries(Entries);
  if (Changed)
  {
    FFiles.setFilter(QString());
  }
  emit directoryChanged();
  return true;
}

void LocalBrowser::cd(const QString & Path)
{
  QString Target = QDir::isAbsolutePath(Path) ? Path : QDir(FRoot).filePath(Path);
  Load(Target);
}

void LocalBrowser::enter(const QString & Name)
{
  Load(path(Name));
}

void LocalBrowser::up()
{
  if (!atRoot())
  {
    Load(QFileInfo(FDirectory).absolutePath());
  }
}

void LocalBrowser::refresh()
{
  if (FAccessible)
  {
    Load(FDirectory);
  }
}

QString LocalBrowser::path(const QString & Name) const
{
  return QDir(FDirectory).filePath(Name);
}

QStringList LocalBrowser::paths(const QStringList & Names) const
{
  QStringList Result;
  for (const QString & Name : Names)
  {
    Result.append(path(Name));
  }
  return Result;
}

bool LocalBrowser::remove(const QStringList & Names)
{
  bool Ok = true;
  for (const QString & Name : Names)
  {
    QFileInfo Info(path(Name));
    bool Done = (Info.isDir() && !Info.isSymLink()) ? QDir(Info.filePath()).removeRecursively() : QFile::remove(Info.filePath());
    if (!Done)
    {
      FLastError = QStringLiteral("Cannot delete %1.").arg(Name);
      Ok = false;
    }
  }
  refresh();
  return Ok;
}

bool LocalBrowser::rename(const QString & From, const QString & To)
{
  if (!ValidName(To) || (From == To))
  {
    FLastError = QStringLiteral("Invalid name.");
    return false;
  }
  if (QFileInfo::exists(path(To)))
  {
    FLastError = QStringLiteral("%1 already exists.").arg(To);
    return false;
  }
  bool Ok = QDir(FDirectory).rename(From, To);
  if (!Ok)
  {
    FLastError = QStringLiteral("Cannot rename %1.").arg(From);
  }
  refresh();
  return Ok;
}

bool LocalBrowser::createFolder(const QString & Name)
{
  if (!ValidName(Name))
  {
    FLastError = QStringLiteral("Invalid name.");
    return false;
  }
  bool Ok = QDir(FDirectory).mkdir(Name);
  if (!Ok)
  {
    FLastError = QStringLiteral("Cannot create the folder %1.").arg(Name);
  }
  refresh();
  return Ok;
}

bool LocalBrowser::createFile(const QString & Name)
{
  if (!ValidName(Name))
  {
    FLastError = QStringLiteral("Invalid name.");
    return false;
  }
  QFile File(path(Name));
  bool Ok = File.open(QIODevice::NewOnly | QIODevice::WriteOnly);
  if (!Ok)
  {
    FLastError = QFileInfo::exists(path(Name)) ? QStringLiteral("%1 already exists.").arg(Name)
                                                : QStringLiteral("Cannot create %1.").arg(Name);
  }
  File.close();
  refresh();
  return Ok;
}

void LocalBrowser::checkAccess()
{
  bool Accessible = AndroidFiles::HasStorageAccess();
  bool Granted = Accessible && !FAccessible;
  if (Accessible != FAccessible)
  {
    FAccessible = Accessible;
    emit accessChanged();
  }
  if (Granted)
  {
    // just allowed: to Download (the root was all that could be seen before)
    QString Downloads = QDir(FRoot).filePath(QStringLiteral("Download"));
    Load(QDir(Downloads).exists() ? Downloads : FRoot);
  }
  else if (FAccessible)
  {
    refresh();
  }
}

void LocalBrowser::requestAccess()
{
  AndroidFiles::RequestStorageAccess();
}
