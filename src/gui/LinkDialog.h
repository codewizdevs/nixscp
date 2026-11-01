// Port of WinSCP source/forms/Symlink.{cpp,dfm} (TSymlinkDialog): add or
// edit a symbolic link, or add a hard link.
// Linux: the local panel creates real symbolic/hard links (symlink(2) /
// link(2)) instead of Windows .lnk shortcuts, so the "/shortcut" wording of
// the WinSCP captions is dropped and hard links are allowed locally too.
#pragma once

#include <QDialog>

class LinkDialog : public QDialog
{
  Q_OBJECT

public:
  explicit LinkDialog(QWidget * Parent = nullptr);

  // DoSymlinkDialog: Edit makes the link name read-only (an existing link
  // gets a new target); AllowHardLink enables "&Hard link" (fcHardLink, not when editing)
  bool Execute(QString & FileName, QString & PointTo, bool & SymbolicLink, bool Edit, bool AllowHardLink);

  QString FileName() const;
  void SetFileName(const QString & Value);
  QString PointTo() const;
  void SetPointTo(const QString & Value);
  bool SymbolicLink() const;
  void SetSymbolicLink(bool Value);
  void SetEdit(bool Value);
  void SetAllowHardLink(bool Value);

protected:
  void showEvent(QShowEvent * Event) override;

private:
  struct Private;
  Private * d;
};
