// Port of WinSCP source/forms/FileSystemInfo.{cpp,dfm} (TFileSystemInfoDialog,
// DoFileSystemInfoDialog): "Server and protocol information".
//
// Protocol page: remote system, file transfer protocol, cryptographic
// protocol, SSH implementation, encryption algorithm, compression; server
// host key fingerprints (SSH) or certificate fingerprints (FTPS, WebDAV/S3
// over HTTPS). Capabilities page: the IsCapable table (TFSCapability) and
// "Additional information" (the remaining lines of the protocol's server
// information, e.g. MAC/key exchange algorithms, SFTP version and extensions,
// FTP features). Space available page (when a GetSpaceAvailable callback is
// given). Copy to Clipboard, Ctrl+C / context menu Copy on the lists, Help.
// The data comes from Terminal::ProtocolName(), Terminal::ServerInfo()
// ("Name: value" lines, see SecureShell::SessionInfo and the FileSystem
// ServerInfo implementations) and Terminal::IsCapable.
// Not ported: "Full certificate" (the TLS layers keep only the fingerprints).
#pragma once

#include "core/FileSystem.h"

#include <QDialog>
#include <QString>

#include <functional>

class QGroupBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QTreeWidget;
class Terminal;

class ServerInfoDialog : public QDialog
{
  Q_OBJECT

public:
  // TSpaceAvailable
  struct SpaceAvailable
  {
    qint64 BytesOnDevice = 0;
    qint64 UnusedBytesOnDevice = 0;
    qint64 BytesAvailableToUser = 0;
    qint64 UnusedBytesAvailableToUser = 0;
    qint64 BytesPerAllocationUnit = 0;
  };
  // TGetSpaceAvailable; false + Error on failure
  using GetSpaceAvailableFunc = std::function<bool(const QString & Path, SpaceAvailable & Result, QString & Error)>;

  // DoFileSystemInfoDialog for an open session
  static void Execute(QWidget * Parent, Terminal * ATerminal, const QString & SpaceAvailablePath = QString(),
    GetSpaceAvailableFunc OnGetSpaceAvailable = GetSpaceAvailableFunc());

  ServerInfoDialog(QWidget * Parent, const QString & ProtocolName, const QString & ServerInfo,
    std::function<bool(FSCapability)> IsCapable, const QString & SpaceAvailablePath = QString(),
    GetSpaceAvailableFunc OnGetSpaceAvailable = GetSpaceAvailableFunc());
  ~ServerInfoDialog() override;

  // ClipboardButtonClick text
  QString ClipboardText();

  // For tests
  QTreeWidget * ServerView() const;
  QTreeWidget * ProtocolView() const;
  QTreeWidget * SpaceAvailableView() const;
  QGroupBox * HostKeyGroup() const;
  QGroupBox * CertificateGroup() const;
  QGroupBox * InfoGroup() const;
  QTabWidget * PageControl() const;
  // Value of a row of a list ("Item" caption), empty when not shown
  static QString ListValue(QTreeWidget * View, const QString & Item);
  QString HostKeyFingerprintSHA256() const;
  QString CertificateFingerprintSHA256() const;
  QString AdditionalInfo() const;

private:
  struct Private;
  Private * d;
};
