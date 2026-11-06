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
  // a key is being generated (RSA takes a while on a phone)
  Q_PROPERTY(bool generating READ generating NOTIFY generatingChanged)

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
  // Type: "ed25519" or "rsa"; generated(name, error) when it is done
  Q_INVOKABLE void generate(const QString & Name, const QString & Type, const QString & Passphrase);
  bool generating() const { return FGenerating; }
  // "ssh-ed25519 AAAA... comment"; "" when it cannot be read without the passphrase
  Q_INVOKABLE QString publicKey(const QString & Name) const;
  Q_INVOKABLE void remove(const QString & Name);
  Q_INVOKABLE QString lastError() const { return FLastError; }

  // With NixSCP Pro's encryption on (the key is known), the private keys
  // are stored encrypted with it (core/ConfigVault). A decrypted key exists
  // in memory only: the connection opens the file itself (core/SecureShell).
  static QString Folder();
  // encrypts the keys that are not (yet); a key is replaced only by its
  // complete encrypted file
  static void SealAll();

  // known_hosts with the host names hashed, as OpenSSH's HashKnownHosts:
  // libssh reads the file at every connection and reconnection (on the
  // connection's thread, a changed key must still be told from an unknown
  // one), so it cannot be sealed, but it no longer tells which hosts these are
  static void HashKnownHosts(const QString & FileName);

signals:
  void changed();
  void generatingChanged();
  // name: the new key, "" with the reason in error
  void generated(const QString & name, const QString & error);

private:
  QString FLastError;
  bool FGenerating = false;
  QString UniqueName(const QString & Wanted) const;
  void Generated(const QString & Name, const QByteArray & Private, const QString & Public, const QString & Error);
};
