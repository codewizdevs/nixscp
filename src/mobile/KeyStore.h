// The SSH private keys of the phone: picked from the phone's files (copied
// into the app's private folder) or generated here (Ed25519 or RSA, OpenSSH
// format, optionally with a passphrase). A site uses one by its path
// (PublicKeyFile, as on desktop); the public key can be copied, shared or
// installed on a server.
#pragma once

#include <QObject>
#include <QVariantList>

class KeyStore : public QObject
{
  Q_OBJECT
  Q_PROPERTY(QVariantList keys READ keys NOTIFY changed)

public:
  explicit KeyStore(QObject * Parent = nullptr);

  // [{name, path}]
  QVariantList keys() const;
  Q_INVOKABLE QString folder() const;
  Q_INVOKABLE QString path(const QString & Name) const;
  // the key's name for a path in the store ("" for a path elsewhere)
  Q_INVOKABLE QString nameOf(const QString & Path) const;
  // A key file (path, file:// or content:// URL); its name, "" with lastError
  Q_INVOKABLE QString importKey(const QString & Source);
  // Type: "ed25519" or "rsa"; its name, "" with lastError
  Q_INVOKABLE QString generate(const QString & Name, const QString & Type, const QString & Passphrase);
  // "ssh-ed25519 AAAA... comment"; "" when it cannot be read without the passphrase
  Q_INVOKABLE QString publicKey(const QString & Name) const;
  Q_INVOKABLE void remove(const QString & Name);
  Q_INVOKABLE QString lastError() const { return FLastError; }

  // With NixSCP Pro's encryption on (the key is known), the private keys
  // are stored encrypted with it (core/ConfigVault); a connection gets a
  // decrypted copy in a private temporary file, removed when it closes.
  static QString Folder();
  // encrypts the keys that are not (yet)
  static void SealAll();
  // a file libssh can read: Path itself, or a decrypted temporary copy
  // ("" when it is encrypted and locked)
  static QString PlainKeyFile(const QString & Path);
  static void RemoveTemporary(const QString & Path);
  // the temporary copies left by an earlier run
  static void CleanTemporary();

signals:
  void changed();

private:
  QString FLastError;
  QString UniqueName(const QString & Wanted) const;
};
