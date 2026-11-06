// test_mobile only: desktop Qt packages come without Qt's private headers.
// RemoteSession::ZipLocally is not run on the desktop; it runs on a device.
#pragma once

#include <QIODevice>
#include <QString>

class QZipWriter
{
public:
  enum Status { NoError };
  enum CompressionPolicy { AutoCompress };

  explicit QZipWriter(const QString &) {}
  void setCompressionPolicy(CompressionPolicy) {}
  void addDirectory(const QString &) {}
  void addFile(const QString &, QIODevice *) {}
  void close() {}
  Status status() const { return NoError; }
};
