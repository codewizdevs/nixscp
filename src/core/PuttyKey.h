// PuTTY private key (.ppk, format versions 2 and 3) loader and writer.
// Port of the PPK part of WinSCP source/putty/sshpubk.c,
// converting the key into a libssh ssh_key and back.
#pragma once

#include <QByteArray>
#include <QString>
#include <functional>
#include <libssh/libssh.h>

namespace PuttyKey
{
  enum LoadResult { lrOk, lrCancelled, lrError };

  bool IsPuttyKeyFile(const QString & FileName);

  // PromptPassphrase is called for encrypted keys (possibly repeatedly after a
  // wrong passphrase, at most 3 times); return false to cancel.
  // On lrOk, *Key receives a new key the caller must ssh_key_free().
  LoadResult Load(const QString & FileName, const std::function<bool(QString & Passphrase, bool Retry)> & PromptPassphrase,
    ssh_key * Key, QString * Error);

  // Comment line of a .ppk file (it is not encrypted)
  QString ReadComment(const QString & FileName);

  // ppk_save_sb: Key as .ppk format 3, encrypted with Argon2id with a
  // Passphrase, or format 2 where OpenSSL has no Argon2. The file is replaced
  // only once written. False with Error when the key type or file is refused.
  bool Save(const QString & FileName, ssh_key Key, const QString & Comment, const QString & Passphrase, QString * Error);

  // The same from the contents of a key file (a key kept encrypted, opened in memory)
  LoadResult LoadData(const QByteArray & FileData, const std::function<bool(QString & Passphrase, bool Retry)> & PromptPassphrase,
    ssh_key * Key, QString * Error);
}
