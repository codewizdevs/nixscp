// Port of WinSCP source/forms/RemoteTransfer.{cpp,dfm} (TRemoteTransferDialog,
// "Duplicate" with target session, target path + mask and "Duplicate via
// local temporary copy") and the TRemoteMoveDialog of source/forms/Custom.cpp
// ("Move" with target path + mask). Both remember the target in the
// RemoteTarget history and confirm transferring several files to one name.
#pragma once

#include "core/Terminal.h"

#include <QDialog>

#include <functional>

class HistoryComboBox;

// TDirectoryExistsEvent: whether Path is an existing directory of Session
// (nullptr = the current session)
using DirectoryExistsFunc = std::function<bool(void * Session, const QString & Path)>;

class RemoteTransferDialog : public QDialog
{
  Q_OBJECT

public:
  // One open session offered in "Target session" (name, its remote directory)
  struct SessionEntry
  {
    QString Name;
    QString Directory;
    void * Session = nullptr;
  };

  explicit RemoteTransferDialog(QWidget * Parent = nullptr);

  // TRemoteTransferDialog::Init
  void Init(bool Multi, const QList<SessionEntry> & Sessions, Terminal::DirectRemoteCopy AllowDirectCopy,
    void * CurrentSession, DirectoryExistsFunc OnDirectoryExists, bool TargetConfirmed = false);
  // TRemoteTransferDialog::Execute: Target is a directory, FileMask the
  // name mask ("*.*"); DirectCopy false = via local temporary copy
  bool Execute(void *& Session, QString & Target, QString & FileMask, bool & DirectCopy);

  // Controls for tests
  HistoryComboBox * DirectoryEdit() const;

  // validates (TFormCloseQuery) before closing
  void accept() override;

private:
  struct Private;
  Private * d;
};

class RemoteMoveDialog : public QDialog
{
  Q_OBJECT

public:
  RemoteMoveDialog(bool Multi, DirectoryExistsFunc OnDirectoryExists, QWidget * Parent = nullptr);

  bool Execute(QString & Target, QString & FileMask);

  HistoryComboBox * DirectoryEdit() const;

  // validates (TFormCloseQuery) before closing
  void accept() override;

private:
  struct Private;
  Private * d;
};
