// PuTTY private key (.ppk, format versions 2 and 3) loader.
// Port of the PPK reading part of WinSCP source/putty/sshpubk.c,
// converting the key into a libssh ssh_key.
#pragma once

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
}
