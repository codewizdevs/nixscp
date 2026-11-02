// Port of WinSCP source/core/SecureShell.cpp (libssh based); the proxy
// negotiation follows PuTTY's proxy/*.c (SOCKS 4/4A/5, HTTP CONNECT, Telnet),
// the public key installation TTerminal::UploadPublicKey (core/Terminal.cpp)
// and the certificate auto-detection AddMatchingKeyCertificate
// (windows/Tools.cpp).
#include "core/SecureShell.h"
#include "core/PuttyKey.h"
#include "core/SessionLog.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QMutex>
#include <QNetworkProxyFactory>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include <libssh/callbacks.h>
#include <libssh/sftp.h>

#include "core/Sockets.h"

#include <fcntl.h>
#ifndef Q_OS_WIN
#include <sys/un.h>
#endif
#include <cerrno>
#include <cstring>
#include <list>

namespace
{

QString SshErrorText(ssh_session Session)
{
  return (Session != nullptr) ? QString::fromUtf8(ssh_get_error(Session)) : QString();
}

QString ExpandHome(const QString & FileName)
{
  QString Result = QDir::fromNativeSeparators(FileName);
  if (Result.startsWith(QStringLiteral("~/")))
  {
    Result = QDir::homePath() + Result.mid(1);
  }
  return Result;
}

// SSH_AUTH_SOCK of the ssh-agent (Linux counterpart of Pageant)
QByteArray AgentSocketPath()
{
  return qgetenv("SSH_AUTH_SOCK");
}

// Credentials for one authentication (the session itself or the tunnel host)
struct AuthSettings
{
  QString SessionName;
  QString UserName;
  QString Password;
  QString PublicKeyFile;
  QString DetachedCertificate;
  bool TryAgent = true;
  bool AuthKI = true;
  bool AuthKIPassword = true;
  bool AuthGSSAPI = false;
  bool NoUserAuth = false;
};

// Authentication part of TSecureShell (PuTTY's ssh2 userauth driven by
// WinSCP's prompts): none -> public key (file or agent/default keys) ->
// GSSAPI -> keyboard-interactive -> password
class Authenticator
{
public:
  Authenticator(ssh_session Session, const AuthSettings & Settings, UserInterface * UI, bool & PasswordUsed) :
    FSession(Session), FSettings(Settings), FUI(UI), FPasswordUsed(PasswordUsed)
  {
  }

  void Authenticate()
  {
    int Rc = ssh_userauth_none(FSession, nullptr);
    if (const char * Banner = ssh_get_issue_banner(FSession))
    {
      QString Text = QString::fromUtf8(Banner);
      ssh_string_free_char(const_cast<char *>(Banner));
      if (!Text.trimmed().isEmpty())
      {
        FUI->DisplayBanner(FSettings.SessionName, Text);
      }
    }
    if (Rc == SSH_AUTH_SUCCESS)
    {
      return;
    }
    if (Rc == SSH_AUTH_ERROR)
    {
      Error(QStringLiteral("Authentication failed."));
    }
    if (FSettings.NoUserAuth)
    {
      // SshNoUserAuth: only the "none" method is attempted
      throw FatalException(QStringLiteral("Authentication failed."), SshErrorText(FSession));
    }

    int Methods = ssh_userauth_list(FSession, nullptr);

    if (Methods & SSH_AUTH_METHOD_PUBLICKEY)
    {
      if (!FSettings.PublicKeyFile.isEmpty())
      {
        // As PuTTY: keys of the agent (Pageant there, ssh-agent here) are
        // tried before the configured key file
        if (FSettings.TryAgent && !AgentSocketPath().isEmpty())
        {
          Rc = ssh_userauth_agent(FSession, nullptr);
          if (Rc == SSH_AUTH_SUCCESS)
          {
            return;
          }
          if ((Rc == SSH_AUTH_ERROR) && !ssh_is_connected(FSession))
          {
            Error(QStringLiteral("Authentication failed."));
          }
        }
        if (AuthenticatePublicKeyFile())
        {
          return;
        }
      }
      else if (FSettings.TryAgent)
      {
        // Agent + default keys (~/.ssh/id_*), like OpenSSH
        Rc = ssh_userauth_publickey_auto(FSession, nullptr, nullptr);
        if (Rc == SSH_AUTH_SUCCESS)
        {
          return;
        }
        if (Rc == SSH_AUTH_ERROR)
        {
          Error(QStringLiteral("Authentication failed."));
        }
      }
    }

    if ((Methods & SSH_AUTH_METHOD_GSSAPI_MIC) && FSettings.AuthGSSAPI)
    {
      Rc = ssh_userauth_gssapi(FSession);
      if (Rc == SSH_AUTH_SUCCESS)
      {
        return;
      }
      if (Rc == SSH_AUTH_ERROR)
      {
        // GSSAPI unavailable (no credentials or no library support) is not fatal
        if (!ssh_is_connected(FSession))
        {
          Error(QStringLiteral("Authentication failed."));
        }
      }
      Methods = ssh_userauth_list(FSession, nullptr);
    }

    if ((Methods & SSH_AUTH_METHOD_INTERACTIVE) && FSettings.AuthKI)
    {
      if (AuthenticateKeyboardInteractive())
      {
        return;
      }
    }

    // Methods may change after partial success
    Methods = ssh_userauth_list(FSession, nullptr);
    if (Methods & SSH_AUTH_METHOD_PASSWORD)
    {
      if (AuthenticatePassword())
      {
        return;
      }
    }

    throw FatalException(QStringLiteral("Authentication failed."),
      QStringLiteral("No supported authentication methods available (server sent: %1)")
        .arg(SshErrorText(FSession)));
  }

private:
  ssh_session FSession;
  AuthSettings FSettings;
  UserInterface * FUI;
  bool & FPasswordUsed;

  [[noreturn]] void Error(const QString & Message)
  {
    throw FatalException(Message, SshErrorText(FSession));
  }

  // Certificate to use with the private key: the configured
  // DetachedCertificate or an auto-detected "<key>-cert.pub"
  // (TSecureShell::Open / AddMatchingKeyCertificate)
  void AddCertificate(ssh_key Key, const QString & KeyFileName)
  {
    QString CertificateFile = ExpandHome(FSettings.DetachedCertificate);
    bool Explicit = !CertificateFile.isEmpty();
    if (!Explicit)
    {
      CertificateFile = SecureShell::MatchingCertificateFile(KeyFileName);
      if (CertificateFile.isEmpty())
      {
        return;
      }
    }
    ssh_key Certificate = nullptr;
    QByteArray Path = QFile::encodeName(CertificateFile);
    int Rc = ssh_pki_import_cert_file(Path.constData(), &Certificate);
    QString Problem;
    if (Rc != SSH_OK)
    {
      Problem = QStringLiteral("Cannot load certificate from \"%1\".").arg(CertificateFile);
    }
    else if (ssh_pki_copy_cert_to_privkey(Certificate, Key) != SSH_OK)
    {
      Problem = QStringLiteral("Certificate \"%1\" does not match the private key.").arg(CertificateFile);
      if (ssh_key_type(Key) == SSH_KEYTYPE_ED25519)
      {
        // libssh 0.11 fails to compare Ed25519 certificates with their key
        Problem += QStringLiteral("\nThe SSH library cannot use certificates of Ed25519 keys.");
      }
    }
    ssh_key_free(Certificate);
    if (!Problem.isEmpty() && Explicit)
    {
      throw FatalException(Problem);
    }
    // An unusable auto-detected certificate is ignored, as in WinSCP
  }

  bool AuthenticatePublicKeyFile()
  {
    QString FileName = ExpandHome(FSettings.PublicKeyFile);
    ssh_key Key = nullptr;
    QFile KeyFile(FileName);
    if (!KeyFile.open(QIODevice::ReadOnly))
    {
      throw FatalException(QStringLiteral("Unable to open key file \"%1\".").arg(FileName), KeyFile.errorString());
    }
    QByteArray Head = KeyFile.read(32);
    KeyFile.close();
    auto PromptPassphrase = [&](QString & Passphrase) -> bool
    {
      QList<Prompt> Prompts{Prompt{QStringLiteral("&Passphrase for key '%1':").arg(QFileInfo(FileName).fileName()), false, QString()}};
      if (!FUI->PromptUser(UserInterface::pkPassphrase, FSettings.SessionName, QString(), Prompts))
      {
        return false;
      }
      Passphrase = Prompts[0].Result;
      return true;
    };

    if (Head.startsWith("PuTTY-User-Key-File-"))
    {
      // WinSCP's native key format
      QString Error;
      PuttyKey::LoadResult Result = PuttyKey::Load(FileName,
        [&](QString & Passphrase, bool) { return PromptPassphrase(Passphrase); }, &Key, &Error);
      if (Result == PuttyKey::lrCancelled)
      {
        throw AbortException();
      }
      if (Result != PuttyKey::lrOk)
      {
        throw FatalException(QStringLiteral("Unable to load private key from '%1'.").arg(FileName), Error);
      }
    }
    else
    {
      QByteArray Path = FileName.toUtf8();
      int Rc = ssh_pki_import_privkey_file(Path.constData(), nullptr, nullptr, nullptr, &Key);
      for (int Attempt = 0; (Rc != SSH_OK) && (Attempt < 3); Attempt++)
      {
        QString Passphrase;
        if (!PromptPassphrase(Passphrase))
        {
          throw AbortException();
        }
        QByteArray Utf = Passphrase.toUtf8();
        Rc = ssh_pki_import_privkey_file(Path.constData(), Utf.constData(), nullptr, nullptr, &Key);
      }
      if (Rc != SSH_OK)
      {
        throw FatalException(QStringLiteral("File '%1' does not contain private key in known format.").arg(FileName));
      }
    }
    try
    {
      AddCertificate(Key, FileName);
    }
    catch (...)
    {
      ssh_key_free(Key);
      throw;
    }
    int Rc = ssh_userauth_publickey(FSession, nullptr, Key);
    ssh_key_free(Key);
    if (Rc == SSH_AUTH_ERROR)
    {
      Error(QStringLiteral("Authentication failed."));
    }
    return (Rc == SSH_AUTH_SUCCESS);
  }

  bool AuthenticateKeyboardInteractive()
  {
    int Rc = ssh_userauth_kbdint(FSession, nullptr, nullptr);
    while (Rc == SSH_AUTH_INFO)
    {
      QString Name = QString::fromUtf8(ssh_userauth_kbdint_getname(FSession));
      QString Instruction = QString::fromUtf8(ssh_userauth_kbdint_getinstruction(FSession));
      int Count = ssh_userauth_kbdint_getnprompts(FSession);
      QList<Prompt> Prompts;
      for (int I = 0; I < Count; I++)
      {
        char Echo = 0;
        const char * Text = ssh_userauth_kbdint_getprompt(FSession, I, &Echo);
        Prompts.append(Prompt{QString::fromUtf8(Text), Echo != 0, QString()});
      }
      if (Count > 0)
      {
        // As WinSCP does (AuthKIPassword), answer single password prompt with stored password
        bool UseStored =
          (Count == 1) && !Prompts[0].Echo && FSettings.AuthKIPassword &&
          !FSettings.Password.isEmpty() && !FPasswordUsed;
        if (UseStored)
        {
          Prompts[0].Result = FSettings.Password;
          FPasswordUsed = true;
        }
        else if (!FUI->PromptUser(UserInterface::pkKeybInteractive, Name.isEmpty() ? FSettings.SessionName : Name, Instruction, Prompts))
        {
          throw AbortException();
        }
        for (int I = 0; I < Count; I++)
        {
          QByteArray Answer = Prompts[I].Result.toUtf8();
          ssh_userauth_kbdint_setanswer(FSession, I, Answer.constData());
        }
      }
      Rc = ssh_userauth_kbdint(FSession, nullptr, nullptr);
    }
    if (Rc == SSH_AUTH_ERROR)
    {
      Error(QStringLiteral("Authentication failed."));
    }
    return (Rc == SSH_AUTH_SUCCESS);
  }

  bool AuthenticatePassword()
  {
    for (int Attempt = 0; Attempt < 3; Attempt++)
    {
      QString Password;
      if (!FSettings.Password.isEmpty() && !FPasswordUsed)
      {
        Password = FSettings.Password;
        FPasswordUsed = true;
      }
      else
      {
        QList<Prompt> Prompts{Prompt{QStringLiteral("&Password: "), false, QString()}};
        QString Instructions = (Attempt > 0) ? QStringLiteral("Access denied.") : QString();
        if (!FUI->PromptUser(UserInterface::pkPassword, FSettings.SessionName, Instructions, Prompts))
        {
          throw AbortException();
        }
        Password = Prompts[0].Result;
      }
      QByteArray Utf = Password.toUtf8();
      int Rc = ssh_userauth_password(FSession, nullptr, Utf.constData());
      if (Rc == SSH_AUTH_SUCCESS)
      {
        return true;
      }
      if (Rc == SSH_AUTH_ERROR)
      {
        Error(QStringLiteral("Authentication failed."));
      }
    }
    return false;
  }
};

// Normalized fingerprint for comparison with configured SshHostKey values:
// "SHA256:base64" without padding, or lowercase "xx:xx:..." MD5
QString NormalizeFingerprint(QString Str)
{
  Str = Str.trimmed();
  // "ssh-ed25519 255 SHA256:..." / "ssh-rsa 2048 xx:xx:.." -> last word
  int P = Str.lastIndexOf(QLatin1Char(' '));
  if (P >= 0)
  {
    Str = Str.mid(P + 1);
  }
  if (Str.startsWith(QStringLiteral("SHA256:"), Qt::CaseInsensitive))
  {
    Str = Str.mid(7);
    while (Str.endsWith(QLatin1Char('=')))
    {
      Str.chop(1);
    }
    return QStringLiteral("SHA256:") + Str;
  }
  if (Str.startsWith(QStringLiteral("MD5:"), Qt::CaseInsensitive))
  {
    Str = Str.mid(4);
  }
  if ((Str.size() == 47) && (Str.count(QLatin1Char(':')) == 15))
  {
    return Str.toLower();
  }
  // base64 SHA-256 without prefix
  while (Str.endsWith(QLatin1Char('=')))
  {
    Str.chop(1);
  }
  return QStringLiteral("SHA256:") + Str;
}

QString KeyFingerprint(ssh_key Key, ssh_publickey_hash_type Type)
{
  unsigned char * Hash = nullptr;
  size_t Len = 0;
  QString Result;
  if (ssh_get_publickey_hash(Key, Type, &Hash, &Len) == SSH_OK)
  {
    char * Str = ssh_get_fingerprint_hash(Type, Hash, Len);
    Result = QString::fromUtf8(Str);
    ssh_string_free_char(Str);
    ssh_clean_pubkey_hash(&Hash);
  }
  return Result;
}

// Host key verification (TSecureShell::VerifyHostKey): configured fingerprint
// (SshHostKey/TunnelHostKey) or known_hosts + user confirmation.
// Returns normally when accepted, throws otherwise.
SshHostKeyInfo ServerHostKey(ssh_session Session)
{
  ssh_key Key = nullptr;
  if (ssh_get_server_publickey(Session, &Key) != SSH_OK)
  {
    throw FatalException(QStringLiteral("Host key wasn't verified!"), SshErrorText(Session));
  }
  SshHostKeyInfo Result;
  Result.KeyType = QString::fromUtf8(ssh_key_type_to_char(ssh_key_type(Key)));
  Result.SHA256 = KeyFingerprint(Key, SSH_PUBLICKEY_HASH_SHA256);
  Result.MD5 = KeyFingerprint(Key, SSH_PUBLICKEY_HASH_MD5);
  if (Result.MD5.startsWith(QStringLiteral("MD5:"), Qt::CaseInsensitive))
  {
    Result.MD5 = Result.MD5.mid(4);
  }
  ssh_key_free(Key);
  return Result;
}

void VerifySessionHostKey(ssh_session Session, const QString & HostName, int Port, const QString & ConfiguredKeys,
  const QString & SessionName, UserInterface * UI, SshHostKeyInfo * Info = nullptr)
{
  SshHostKeyInfo HostKey = ServerHostKey(Session);
  if (Info != nullptr)
  {
    *Info = HostKey;
  }
  const QString & KeyType = HostKey.KeyType;
  const QString & SHA256 = HostKey.SHA256;
  const QString & MD5 = HostKey.MD5;

  if (!ConfiguredKeys.trimmed().isEmpty())
  {
    // As TSecureShell::VerifyHostKey with SshHostKey set: a matching
    // configured fingerprint is accepted, anything else is rejected
    // without asking; "*" accepts any key.
    QString Ours256 = NormalizeFingerprint(SHA256);
    QString OursMD5 = MD5.toLower();
    if (OursMD5.startsWith(QStringLiteral("md5:")))
    {
      OursMD5 = OursMD5.mid(4);
    }
    for (const QString & Entry : ConfiguredKeys.split(QLatin1Char(';'), Qt::SkipEmptyParts))
    {
      if (Entry.trimmed() == QStringLiteral("*"))
      {
        return;
      }
      QString Normalized = NormalizeFingerprint(Entry);
      if ((Normalized == Ours256) || (Normalized == OursMD5))
      {
        return;
      }
    }
    throw FatalException(QStringLiteral("Host key does not match configured key fingerprint \"%1\"!").arg(ConfiguredKeys.trimmed()),
      QStringLiteral("Host key fingerprint is %1 %2.").arg(KeyType, SHA256));
  }

  ssh_known_hosts_e State = ssh_session_is_known_server(Session);
  if (State == SSH_KNOWN_HOSTS_OK)
  {
    return;
  }
  if (State == SSH_KNOWN_HOSTS_ERROR)
  {
    throw FatalException(QStringLiteral("Host key wasn't verified!"), SshErrorText(Session));
  }

  UserInterface::HostKeyState UIState =
    ((State == SSH_KNOWN_HOSTS_CHANGED) || (State == SSH_KNOWN_HOSTS_OTHER)) ?
      UserInterface::HostKeyChanged : UserInterface::HostKeyUnknown;
  UserInterface::HostKeyAnswer Answer = UI->VerifyHostKey(HostName, Port, KeyType, SHA256, MD5, UIState);
  switch (Answer)
  {
    case UserInterface::HostKeyAccept:
      if (ssh_session_update_known_hosts(Session) != SSH_OK)
      {
        // STORE_NEW_HOSTKEY_ERROR is a warning only in WinSCP
        UI->DisplayBanner(SessionName, QStringLiteral("Failed to store new host key.\n%1").arg(SshErrorText(Session)));
      }
      break;
    case UserInterface::HostKeyOnce:
      break;
    case UserInterface::HostKeyCancel:
      throw FatalException(QStringLiteral("Host key wasn't verified!"));
  }
}

// WinSCP algorithm names (SSH page) -> libssh/OpenSSH algorithm names.
// Algorithms libssh does not implement are left out (libssh drops unknown
// names as long as at least one is supported).
QStringList CipherAlgorithms(const QString & Name)
{
  if (Name == QStringLiteral("aes"))
  {
    return {QStringLiteral("aes256-ctr"), QStringLiteral("aes192-ctr"), QStringLiteral("aes128-ctr"),
            QStringLiteral("aes256-cbc"), QStringLiteral("aes192-cbc"), QStringLiteral("aes128-cbc")};
  }
  if (Name == QStringLiteral("chacha20")) return {QStringLiteral("chacha20-poly1305@openssh.com")};
  if (Name == QStringLiteral("aesgcm")) return {QStringLiteral("aes256-gcm@openssh.com"), QStringLiteral("aes128-gcm@openssh.com")};
  if (Name == QStringLiteral("3des")) return {QStringLiteral("3des-cbc")};
  if (Name == QStringLiteral("blowfish")) return {QStringLiteral("blowfish-cbc")};
  return {}; // des, arcfour: not in libssh
}

QStringList KexAlgorithms(const QString & Name)
{
  if (Name == QStringLiteral("ntru-curve25519")) return {QStringLiteral("sntrup761x25519-sha512"), QStringLiteral("sntrup761x25519-sha512@openssh.com")};
  if (Name == QStringLiteral("mlkem-curve25519")) return {QStringLiteral("mlkem768x25519-sha256")};
  if (Name == QStringLiteral("mlkem-nist")) return {QStringLiteral("mlkem768nistp256-sha256")};
  if (Name == QStringLiteral("ecdh"))
  {
    return {QStringLiteral("curve25519-sha256"), QStringLiteral("curve25519-sha256@libssh.org"),
            QStringLiteral("ecdh-sha2-nistp256"), QStringLiteral("ecdh-sha2-nistp384"), QStringLiteral("ecdh-sha2-nistp521")};
  }
  if (Name == QStringLiteral("dh-gex-sha1"))
  {
    return {QStringLiteral("diffie-hellman-group-exchange-sha256"), QStringLiteral("diffie-hellman-group-exchange-sha1")};
  }
  if (Name == QStringLiteral("dh-group18-sha512")) return {QStringLiteral("diffie-hellman-group18-sha512")};
  if (Name == QStringLiteral("dh-group16-sha512")) return {QStringLiteral("diffie-hellman-group16-sha512")};
  if (Name == QStringLiteral("dh-group14-sha1"))
  {
    return {QStringLiteral("diffie-hellman-group14-sha256"), QStringLiteral("diffie-hellman-group14-sha1")};
  }
  if (Name == QStringLiteral("dh-group1-sha1")) return {QStringLiteral("diffie-hellman-group1-sha1")};
  return {}; // dh-group15/17, rsa: not in libssh
}

QStringList HostKeyAlgorithms(const QString & Name)
{
  if (Name == QStringLiteral("ed25519")) return {QStringLiteral("ssh-ed25519"), QStringLiteral("ssh-ed25519-cert-v01@openssh.com")};
  if (Name == QStringLiteral("ecdsa"))
  {
    return {QStringLiteral("ecdsa-sha2-nistp521"), QStringLiteral("ecdsa-sha2-nistp384"), QStringLiteral("ecdsa-sha2-nistp256"),
            QStringLiteral("ecdsa-sha2-nistp521-cert-v01@openssh.com"), QStringLiteral("ecdsa-sha2-nistp384-cert-v01@openssh.com"),
            QStringLiteral("ecdsa-sha2-nistp256-cert-v01@openssh.com")};
  }
  if (Name == QStringLiteral("rsa"))
  {
    return {QStringLiteral("rsa-sha2-512"), QStringLiteral("rsa-sha2-256"), QStringLiteral("ssh-rsa"),
            QStringLiteral("rsa-sha2-512-cert-v01@openssh.com"), QStringLiteral("rsa-sha2-256-cert-v01@openssh.com")};
  }
  if (Name == QStringLiteral("dsa")) return {QStringLiteral("ssh-dss")};
  return {}; // ed448: not in libssh
}

// The order of the list matters; the WARN position is enforced after the key
// exchange by CheckWeakAlgorithms (algorithms below it stay offered, as in
// WinSCP, but using one needs a confirmation).
QString AlgorithmOption(const QStringList & List, const QStringList & Defaults, QStringList (*Map)(const QString &), bool OnlyIfChanged)
{
  QStringList Order = List;
  Order.removeAll(QStringLiteral("WARN"));
  QStringList DefaultOrder = Defaults;
  DefaultOrder.removeAll(QStringLiteral("WARN"));
  if (OnlyIfChanged && (Order == DefaultOrder))
  {
    return QString();
  }
  QStringList Result;
  for (const QString & Name : Order)
  {
    for (const QString & Algorithm : Map(Name))
    {
      if (!Result.contains(Algorithm))
      {
        Result.append(Algorithm);
      }
    }
  }
  return Result.join(QLatin1Char(','));
}

// True when the negotiated Algorithm belongs to an entry of List placed below
// its "-- warn below here --" separator
bool BelowWarnThreshold(const QStringList & List, QStringList (*Map)(const QString &), const QString & Algorithm)
{
  qsizetype WarnIndex = List.indexOf(QStringLiteral("WARN"));
  if ((WarnIndex < 0) || Algorithm.isEmpty())
  {
    return false;
  }
  for (qsizetype Index = 0; Index < List.size(); Index++)
  {
    if (Map(List[Index]).contains(Algorithm))
    {
      return (Index > WarnIndex);
    }
  }
  return false;
}

// TSecureShell::AskAlg (ALG_BELOW_TRESHOLD2, WEAK_ALG_RISK_CONFIRM)
void AskAlg(UserInterface * UI, const QString & AlgType, const QString & AlgName)
{
  QString Message =
    QStringLiteral("**Security Alert**\n\n") +
    QStringLiteral("The first %1 supported by the server is %2, which is below the configured warning threshold.").arg(AlgType, AlgName) +
    QStringLiteral("\n\nDo you want to accept the risk and continue with this session?");
  QueryUserParams Params;
  Params.Type = qtWarning;
  if (UI->QueryUserEx(Message, QString(), qaYes | qaNo, Params) != qaYes)
  {
    // ALG_NOT_VERIFIED
    throw FatalException(QStringLiteral("The %1 %2 was not verified!").arg(AlgType, AlgName));
  }
}

// PuTTY asks during the key exchange (kex, host key, ciphers); libssh only
// tells the negotiated algorithms afterwards, so the questions come before
// the host key verification
void CheckWeakAlgorithms(ssh_session Session, const SessionData & Data, const QString & HostKeyType, UserInterface * UI)
{
  QString Kex = QString::fromUtf8(ssh_get_kex_algo(Session));
  if (BelowWarnThreshold(Data.Kexes(), KexAlgorithms, Kex))
  {
    AskAlg(UI, QStringLiteral("key-exchange algorithm"), Kex);
  }
  if (BelowWarnThreshold(Data.HostKeys(), HostKeyAlgorithms, HostKeyType))
  {
    AskAlg(UI, QStringLiteral("host key type"), HostKeyType);
  }
  const char * Out = ssh_get_cipher_out(Session);
  const char * In = ssh_get_cipher_in(Session);
  QString CipherCS = QString::fromUtf8((Out != nullptr) ? Out : "");
  QString CipherSC = QString::fromUtf8((In != nullptr) ? In : "");
  bool WarnCS = BelowWarnThreshold(Data.Ciphers(), CipherAlgorithms, CipherCS);
  bool WarnSC = BelowWarnThreshold(Data.Ciphers(), CipherAlgorithms, CipherSC);
  if (WarnCS && WarnSC && (CipherCS == CipherSC))
  {
    AskAlg(UI, QStringLiteral("cipher"), CipherCS);
  }
  else
  {
    if (WarnCS)
    {
      AskAlg(UI, QStringLiteral("client-to-server cipher"), CipherCS);
    }
    if (WarnSC)
    {
      AskAlg(UI, QStringLiteral("server-to-client cipher"), CipherSC);
    }
  }
}

bool IsLocalhost(const QString & Host)
{
  return (Host.compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0) ||
    Host.startsWith(QStringLiteral("127.")) || (Host == QStringLiteral("::1"));
}

// PuTTY format_telnet_command(): %host %port %user %pass %proxyhost %proxyport %%
// and backslash escapes
QString FormatProxyCommand(const QString & Format, const SessionData & Data, const QString & TargetHost)
{
  QString Result;
  for (qsizetype I = 0; I < Format.size(); I++)
  {
    QChar C = Format[I];
    if ((C == QLatin1Char('\\')) && (I + 1 < Format.size()))
    {
      QChar N = Format[++I];
      switch (N.unicode())
      {
        case 'n': Result += QLatin1Char('\n'); break;
        case 'r': Result += QLatin1Char('\r'); break;
        case 't': Result += QLatin1Char('\t'); break;
        case '\\': Result += QLatin1Char('\\'); break;
        case '%': Result += QLatin1Char('%'); break;
        case 'x':
        {
          bool Ok = false;
          int Code = Format.mid(I + 1, 2).toInt(&Ok, 16);
          if (Ok)
          {
            Result += QChar(Code);
            I += 2;
          }
          break;
        }
        default: Result += QLatin1Char('\\'); Result += N; break;
      }
    }
    else if (C == QLatin1Char('%'))
    {
      static const struct { const char * Name; int Field; } Vars[] = {
        {"proxyhost", 0}, {"proxyport", 1}, {"host", 2}, {"port", 3}, {"user", 4}, {"pass", 5}, {"%", 6} };
      bool Found = false;
      for (const auto & Var : Vars)
      {
        QString Name = QString::fromLatin1(Var.Name);
        if (Format.mid(I + 1, Name.size()).compare(Name, Qt::CaseInsensitive) == 0)
        {
          switch (Var.Field)
          {
            case 0: Result += Data.ProxyHost; break;
            case 1: Result += QString::number(Data.ProxyPort); break;
            case 2: Result += TargetHost; break;
            case 3: Result += QString::number(Data.PortNumber); break;
            case 4: Result += Data.ProxyUsername; break;
            case 5: Result += Data.ProxyPassword; break;
            case 6: Result += QLatin1Char('%'); break;
          }
          I += Name.size();
          Found = true;
          break;
        }
      }
      if (!Found)
      {
        Result += C;
      }
    }
    else
    {
      Result += C;
    }
  }
  return Result;
}

[[noreturn]] void ConnectionError(const QString & Host, const QString & More)
{
  throw FatalException(QStringLiteral("Network error: Connection to \"%1\" failed.").arg(Host), More);
}

[[noreturn]] void ProxyError(const QString & Message)
{
  throw FatalException(QStringLiteral("Proxy error: %1").arg(Message));
}

// Resolves Host (PuTTY name_lookup); throws "Host does not exist"
addrinfo * ResolveHost(const QString & Host, int Port, int Family)
{
  addrinfo Hints{};
  Hints.ai_family = Family;
  Hints.ai_socktype = SOCK_STREAM;
  addrinfo * Addresses = nullptr;
  QByteArray HostUtf = Host.toUtf8();
  QByteArray PortStr = QByteArray::number(Port);
  int Rc = getaddrinfo(HostUtf.constData(), PortStr.constData(), &Hints, &Addresses);
  if (Rc != 0)
  {
#ifdef Q_OS_WIN
    QString Reason = QString::fromWCharArray(gai_strerrorW(Rc));
#else
    QString Reason = QString::fromLocal8Bit(gai_strerror(Rc));
#endif
    ConnectionError(Host, QStringLiteral("Host does not exist.\n%1").arg(Reason));
  }
  return Addresses;
}

int AddressFamilyToAF(TAddressFamily Family)
{
  return (Family == afIPv4) ? AF_INET : (Family == afIPv6) ? AF_INET6 : AF_UNSPEC;
}

// Opens a TCP connection with the session timeout, honouring the address
// family and source address; returns the blocking socket
Net::Socket ConnectTcp(const QString & Host, int Port, TAddressFamily Family, const QString & SourceAddress, int TimeoutSec)
{
  Net::Startup();
  addrinfo * Addresses = ResolveHost(Host, Port, AddressFamilyToAF(Family));
  Net::Socket Socket = Net::Invalid;
  int LastError = 0;
  for (addrinfo * A = Addresses; (A != nullptr) && (Socket == Net::Invalid); A = A->ai_next)
  {
    Net::Socket S = Net::Open(A->ai_family, A->ai_socktype, A->ai_protocol);
    if (S == Net::Invalid)
    {
      LastError = Net::LastError();
      continue;
    }
    if (!SourceAddress.isEmpty())
    {
      addrinfo BindHints{};
      BindHints.ai_family = A->ai_family;
      BindHints.ai_socktype = SOCK_STREAM;
      BindHints.ai_flags = AI_PASSIVE;
      addrinfo * Bind = nullptr;
      QByteArray SourceUtf = SourceAddress.toUtf8();
      if ((getaddrinfo(SourceUtf.constData(), nullptr, &BindHints, &Bind) != 0) || (Bind == nullptr) ||
          (::bind(S, Bind->ai_addr, static_cast<int>(Bind->ai_addrlen)) != 0))
      {
        LastError = Net::LastError();
        if (Bind != nullptr)
        {
          freeaddrinfo(Bind);
        }
        Net::Close(S);
        continue;
      }
      freeaddrinfo(Bind);
    }
    // Connect with the session timeout
    Net::SetBlocking(S, false);
    int C = ::connect(S, A->ai_addr, static_cast<int>(A->ai_addrlen));
    int Error = (C != 0) ? Net::LastError() : 0;
    if ((C != 0) && Net::WouldBlock(Error))
    {
      Net::PollFd P{};
      P.fd = S;
      P.events = POLLOUT;
      bool Ready = (Net::Poll(&P, 1, std::max(TimeoutSec, 1) * 1000) == 1);
      Error = Ready ? Net::GetError(S) : Net::TimedOut;
      C = (Error == 0) ? 0 : -1;
    }
    if (C != 0)
    {
      LastError = Error;
      Net::Close(S);
      continue;
    }
    Net::SetBlocking(S, true);
    Socket = S;
  }
  freeaddrinfo(Addresses);
  if (Socket == Net::Invalid)
  {
    ConnectionError(Host, Net::ErrorText(LastError));
  }
  return Socket;
}

// Blocking socket I/O with a timeout, for the proxy negotiation
class ProxyIo
{
public:
  ProxyIo(Net::Socket Socket, int TimeoutSec) :
    FSocket(Socket), FTimeoutMs(std::max(TimeoutSec, 1) * 1000)
  {
  }

  void Send(const QByteArray & Data)
  {
    qsizetype Done = 0;
    while (Done < Data.size())
    {
      Wait(POLLOUT);
      qint64 N = Net::Send(FSocket, Data.constData() + Done, Data.size() - Done, Net::NoSignal);
      if (N < 0)
      {
        int Error = Net::LastError();
        if (Net::Interrupted(Error) || Net::WouldBlock(Error))
        {
          continue;
        }
        ProxyError(Net::ErrorText(Error));
      }
      Done += N;
    }
  }

  QByteArray Receive(qsizetype Len)
  {
    QByteArray Result;
    while (Result.size() < Len)
    {
      Wait(POLLIN);
      char Buffer[1024];
      qint64 N = Net::Receive(FSocket, Buffer, std::min<qsizetype>(sizeof(Buffer), Len - Result.size()), 0);
      if (N == 0)
      {
        ProxyError(QStringLiteral("Proxy server closed the connection unexpectedly."));
      }
      if (N < 0)
      {
        int Error = Net::LastError();
        if (Net::Interrupted(Error) || Net::WouldBlock(Error))
        {
          continue;
        }
        ProxyError(Net::ErrorText(Error));
      }
      Result.append(Buffer, N);
    }
    return Result;
  }

  // Data waiting in the socket, without consuming it
  QByteArray Peek(qsizetype Max)
  {
    Wait(POLLIN);
    QByteArray Result(Max, Qt::Uninitialized);
    qint64 N;
    do
    {
      N = Net::Receive(FSocket, Result.data(), Max, MSG_PEEK);
    }
    while ((N < 0) && Net::Interrupted(Net::LastError()));
    if (N <= 0)
    {
      ProxyError(QStringLiteral("Proxy server closed the connection unexpectedly."));
    }
    Result.truncate(N);
    return Result;
  }

  // HTTP response header, up to and including the empty line
  QByteArray ReceiveHeader()
  {
    QByteArray Result;
    while (!Result.endsWith("\r\n\r\n") && !Result.endsWith("\n\n"))
    {
      if (Result.size() > 65536)
      {
        ProxyError(QStringLiteral("HTTP response too long."));
      }
      Result += Receive(1);
    }
    return Result;
  }

  void Wait(short Events)
  {
    Net::PollFd P{};
    P.fd = FSocket;
    P.events = Events;
    int Rc;
    do
    {
      Rc = Net::Poll(&P, 1, FTimeoutMs);
    }
    while ((Rc < 0) && Net::Interrupted(Net::LastError()));
    if (Rc == 0)
    {
      ProxyError(QStringLiteral("Timeout while waiting for the proxy server."));
    }
    if (Rc < 0)
    {
      ProxyError(Net::ErrorText(Net::LastError()));
    }
  }

  int TimeoutMs() const { return FTimeoutMs; }

private:
  Net::Socket FSocket;
  int FTimeoutMs;
};

// Literal IPv4/IPv6 address in network byte order (empty if Host is a name)
QByteArray LiteralAddress(const QString & Host, int & Family)
{
  QByteArray HostUtf = Host.toUtf8();
  in_addr V4;
  if (inet_pton(AF_INET, HostUtf.constData(), &V4) == 1)
  {
    Family = AF_INET;
    return QByteArray(reinterpret_cast<const char *>(&V4), sizeof(V4));
  }
  QByteArray Stripped = HostUtf;
  if (Stripped.startsWith('[') && Stripped.endsWith(']'))
  {
    Stripped = Stripped.mid(1, Stripped.size() - 2);
  }
  in6_addr V6;
  if (inet_pton(AF_INET6, Stripped.constData(), &V6) == 1)
  {
    Family = AF_INET6;
    return QByteArray(reinterpret_cast<const char *>(&V6), sizeof(V6));
  }
  Family = AF_UNSPEC;
  return QByteArray();
}

// Local DNS lookup of the target host (ProxyDNS off, or SOCKS 4 without 4A)
QByteArray ResolveAddress(const QString & Host, int Port, int RequestFamily, int & Family)
{
  addrinfo * Addresses = ResolveHost(Host, Port, RequestFamily);
  QByteArray Result;
  for (addrinfo * A = Addresses; (A != nullptr) && Result.isEmpty(); A = A->ai_next)
  {
    if (A->ai_family == AF_INET)
    {
      const sockaddr_in * In = reinterpret_cast<const sockaddr_in *>(A->ai_addr);
      Result = QByteArray(reinterpret_cast<const char *>(&In->sin_addr), sizeof(In->sin_addr));
      Family = AF_INET;
    }
    else if (A->ai_family == AF_INET6)
    {
      const sockaddr_in6 * In = reinterpret_cast<const sockaddr_in6 *>(A->ai_addr);
      Result = QByteArray(reinterpret_cast<const char *>(&In->sin6_addr), sizeof(In->sin6_addr));
      Family = AF_INET6;
    }
  }
  freeaddrinfo(Addresses);
  if (Result.isEmpty())
  {
    ConnectionError(Host, QStringLiteral("Host does not exist."));
  }
  return Result;
}

QString AddressToString(const QByteArray & Address, int Family)
{
  char Buffer[INET6_ADDRSTRLEN] = {};
  inet_ntop(Family, Address.constData(), Buffer, sizeof(Buffer));
  return QString::fromLatin1(Buffer);
}

QByteArray PortBytes(int Port)
{
  QByteArray Result(2, 0);
  Result[0] = static_cast<char>((Port >> 8) & 0xFF);
  Result[1] = static_cast<char>(Port & 0xFF);
  return Result;
}

// PuTTY proxy/socks4.c: SOCKS 4, or 4A when the proxy resolves the name
void NegotiateSocks4(ProxyIo & Io, const SessionData & Data, bool DnsAtProxy)
{
  int Family = AF_UNSPEC;
  QByteArray Address = LiteralAddress(Data.HostName, Family);
  bool Socks4A = false;
  if (Family != AF_INET)
  {
    if (Family == AF_INET6)
    {
      ProxyError(QStringLiteral("SOCKS version 4 does not support IPv6."));
    }
    if (DnsAtProxy)
    {
      Socks4A = true;
      Address = QByteArray("\0\0\0\x01", 4);
    }
    else
    {
      Address = ResolveAddress(Data.HostName, Data.PortNumber, AF_INET, Family);
    }
  }
  QByteArray Request;
  Request += '\x04';
  Request += '\x01';
  Request += PortBytes(Data.PortNumber);
  Request += Address;
  Request += Data.ProxyUsername.toUtf8();
  Request += '\0';
  if (Socks4A)
  {
    Request += Data.HostName.toUtf8();
    Request += '\0';
  }
  Io.Send(Request);
  QByteArray Reply = Io.Receive(8);
  if (Reply[0] != 0)
  {
    ProxyError(QStringLiteral("SOCKS proxy response contained reply version number %1 (expected 0)").arg(static_cast<unsigned char>(Reply[0])));
  }
  switch (static_cast<unsigned char>(Reply[1]))
  {
    case 90:
      return;
    case 92:
      ProxyError(QStringLiteral("SOCKS server wanted IDENTD on client"));
    case 93:
      ProxyError(QStringLiteral("Username and IDENTD on client don't agree"));
    default:
      ProxyError(QStringLiteral("SOCKS server reported request rejected or failed"));
  }
}

// PuTTY proxy/socks5.c (RFC 1928, username/password RFC 1929)
void NegotiateSocks5(ProxyIo & Io, const SessionData & Data, bool DnsAtProxy)
{
  bool HaveAuth = !Data.ProxyUsername.isEmpty();
  QByteArray Greeting("\x05", 1);
  Greeting += HaveAuth ? QByteArray("\x02\x00\x02", 3) : QByteArray("\x01\x00", 2);
  Io.Send(Greeting);
  QByteArray Reply = Io.Receive(2);
  if (Reply[0] != 5)
  {
    ProxyError(QStringLiteral("SOCKS proxy returned unexpected version %1").arg(static_cast<unsigned char>(Reply[0])));
  }
  unsigned char Method = static_cast<unsigned char>(Reply[1]);
  if (Method == 0x02)
  {
    QByteArray User = Data.ProxyUsername.toUtf8().left(255);
    QByteArray Password = Data.ProxyPassword.toUtf8().left(255);
    QByteArray Auth;
    Auth += '\x01';
    Auth += static_cast<char>(User.size());
    Auth += User;
    Auth += static_cast<char>(Password.size());
    Auth += Password;
    Io.Send(Auth);
    QByteArray AuthReply = Io.Receive(2);
    if (AuthReply[1] != 0)
    {
      // PROXY_AUTHENTICATION_FAILED
      throw FatalException(QStringLiteral("Proxy authentication failed."));
    }
  }
  else if (Method == 0xFF)
  {
    ProxyError(HaveAuth ? QStringLiteral("SOCKS server rejected every authentication method")
      : QStringLiteral("SOCKS server wanted authentication, but no proxy username is configured"));
  }
  else if (Method != 0x00)
  {
    ProxyError(QStringLiteral("SOCKS proxy asked for an unsupported authentication method %1").arg(Method));
  }

  QByteArray Request("\x05\x01\x00", 3);
  int Family = AF_UNSPEC;
  QByteArray Address = LiteralAddress(Data.HostName, Family);
  if ((Family == AF_UNSPEC) && !DnsAtProxy)
  {
    Address = ResolveAddress(Data.HostName, Data.PortNumber, AddressFamilyToAF(Data.AddressFamily), Family);
  }
  if (Family == AF_INET)
  {
    Request += '\x01';
    Request += Address;
  }
  else if (Family == AF_INET6)
  {
    Request += '\x04';
    Request += Address;
  }
  else
  {
    QByteArray Name = Data.HostName.toUtf8();
    if (Name.size() > 255)
    {
      ProxyError(QStringLiteral("Host name too long for SOCKS 5"));
    }
    Request += '\x03';
    Request += static_cast<char>(Name.size());
    Request += Name;
  }
  Request += PortBytes(Data.PortNumber);
  Io.Send(Request);

  QByteArray Head = Io.Receive(4);
  if (Head[0] != 5)
  {
    ProxyError(QStringLiteral("SOCKS proxy returned unexpected version %1").arg(static_cast<unsigned char>(Head[0])));
  }
  unsigned char Status = static_cast<unsigned char>(Head[1]);
  if (Status != 0)
  {
    static const char * const Errors[] = {
      "", "general SOCKS server failure", "connection not allowed by ruleset", "network unreachable",
      "host unreachable", "connection refused", "TTL expired", "command not supported", "address type not supported" };
    QString Text = (Status < std::size(Errors)) ? QString::fromLatin1(Errors[Status]) : QStringLiteral("unrecognised SOCKS error code %1").arg(Status);
    ProxyError(QStringLiteral("SOCKS proxy reported %1").arg(Text));
  }
  // Bound address
  switch (static_cast<unsigned char>(Head[3]))
  {
    case 0x01: Io.Receive(4 + 2); break;
    case 0x04: Io.Receive(16 + 2); break;
    case 0x03: Io.Receive(static_cast<unsigned char>(Io.Receive(1)[0]) + 2); break;
    default: ProxyError(QStringLiteral("SOCKS proxy response contained unrecognised address format"));
  }
}

// PuTTY proxy/http.c: CONNECT with Basic authentication. Returns false when
// the proxy asks for (other) credentials (407).
bool NegotiateHttp(ProxyIo & Io, const SessionData & Data, bool DnsAtProxy, QString & StatusLine)
{
  QString Target = Data.HostName;
  int Family = AF_UNSPEC;
  LiteralAddress(Target, Family);
  if ((Family == AF_UNSPEC) && !DnsAtProxy)
  {
    QByteArray Address = ResolveAddress(Target, Data.PortNumber, AddressFamilyToAF(Data.AddressFamily), Family);
    Target = AddressToString(Address, Family);
  }
  if ((Family == AF_INET6) && !Target.startsWith(QLatin1Char('[')))
  {
    Target = QStringLiteral("[%1]").arg(Target);
  }
  QString HostPort = QStringLiteral("%1:%2").arg(Target).arg(Data.PortNumber);
  QByteArray Request = QStringLiteral("CONNECT %1 HTTP/1.1\r\nHost: %1\r\n").arg(HostPort).toUtf8();
  if (!Data.ProxyUsername.isEmpty())
  {
    QByteArray Credentials = (Data.ProxyUsername + QLatin1Char(':') + Data.ProxyPassword).toUtf8().toBase64();
    Request += "Proxy-Authorization: Basic " + Credentials + "\r\n";
  }
  Request += "\r\n";
  Io.Send(Request);
  QByteArray Header = Io.ReceiveHeader();
  StatusLine = QString::fromLatin1(Header.left(Header.indexOf('\n'))).trimmed();
  QStringList Parts = StatusLine.split(QLatin1Char(' '), Qt::SkipEmptyParts);
  if ((Parts.size() < 2) || !Parts[0].startsWith(QStringLiteral("HTTP/")))
  {
    ProxyError(QStringLiteral("HTTP response was absent"));
  }
  int Status = Parts[1].toInt();
  if (Status == 407)
  {
    return false;
  }
  if ((Status < 200) || (Status > 299))
  {
    ProxyError(StatusLine.mid(Parts[0].size() + 1));
  }
  return true;
}

// PuTTY proxy/telnet.c: sends the configured command; what the proxy prints
// before the server's "SSH-" identification is skipped (PuTTY's SSH layer
// ignores such lines, libssh would not)
void NegotiateTelnet(ProxyIo & Io, const SessionData & Data, const QString & Target)
{
  QString Command = FormatProxyCommand(Data.ProxyTelnetCommand, Data, Target);
  Io.Send(Command.toUtf8());
  QElapsedTimer Timer;
  Timer.start();
  qsizetype LastSize = -1;
  for (;;)
  {
    QByteArray Pending = Io.Peek(8192);
    if (Pending.startsWith("SSH-"))
    {
      return;
    }
    qsizetype Pos = Pending.indexOf("\nSSH-");
    if (Pos >= 0)
    {
      Io.Receive(Pos + 1);
      return;
    }
    qsizetype NewLine = Pending.lastIndexOf('\n');
    if (NewLine >= 0)
    {
      Io.Receive(NewLine + 1);
      LastSize = -1;
      continue;
    }
    if (Pending.size() >= 8192)
    {
      ProxyError(QStringLiteral("Unexpected response from the Telnet proxy."));
    }
    if (Pending.size() == LastSize)
    {
      // Partial line: wait for more data
      if (Timer.elapsed() > Io.TimeoutMs())
      {
        ProxyError(QStringLiteral("Timeout while waiting for the proxy server."));
      }
      usleep(10000);
    }
    LastSize = Pending.size();
  }
}

// Connection > Proxy applies to this connection (PuTTY proxy_for_destination)
bool UsesProxy(const SessionData & Data)
{
  if (Data.ProxyMethod == pmNone)
  {
    return false;
  }
  return !IsLocalhost(Data.HostName) || Data.ProxyLocalhost;
}

// ProxyDNS: names are resolved by the proxy (Auto: for all proxy types but
// SOCKS 4, as in PuTTY)
bool DnsAtProxy(const SessionData & Data)
{
  return (Data.ProxyDNS == asOn) || ((Data.ProxyDNS == asAuto) && (Data.ProxyMethod != pmSocks4));
}

}

// ---------------------------------------------------------------- SecureShell

// Tunnel (Connection > Tunnel) through libssh's ProxyJump; libssh runs the
// jump connection in its own thread and calls these back from there
struct SecureShell::JumpData
{
  SecureShell * Owner = nullptr;
  AuthSettings Auth;
  QString HostName;
  int Port = SshPortNumber;
  QString HostKey;
  bool PasswordUsed = false;
  QString Error;
  QString MoreMessages;
  bool Aborted = false;
  ssh_jump_callbacks_struct Callbacks{};

  static int BeforeConnection(ssh_session Session, void * UserData)
  {
    JumpData * Self = static_cast<JumpData *>(UserData);
    const SessionData & Data = Self->Owner->FData;
    long Timeout = Data.Timeout;
    ssh_options_set(Session, SSH_OPTIONS_TIMEOUT, &Timeout);
    if (!Data.KnownHostsFile.isEmpty())
    {
      QByteArray KnownHosts = QFile::encodeName(Data.KnownHostsFile);
      ssh_options_set(Session, SSH_OPTIONS_KNOWNHOSTS, KnownHosts.constData());
    }
    return 0;
  }

  template <typename F>
  int Guard(F Function)
  {
    try
    {
      Function();
      return 0;
    }
    catch (const AbortException &)
    {
      Aborted = true;
    }
    catch (const ExtException & E)
    {
      Error = E.Message();
      MoreMessages = E.MoreMessages();
    }
    catch (...)
    {
      Error = QStringLiteral("Error while tunneling the connection.");
    }
    return -1;
  }

  static int VerifyKnownHost(ssh_session Session, void * UserData)
  {
    JumpData * Self = static_cast<JumpData *>(UserData);
    return Self->Guard([&]()
    {
      VerifySessionHostKey(Session, Self->HostName, Self->Port, Self->HostKey, Self->Auth.SessionName, Self->Owner->FUI);
    });
  }

  static int Authenticate(ssh_session Session, void * UserData)
  {
    JumpData * Self = static_cast<JumpData *>(UserData);
    return Self->Guard([&]()
    {
      Authenticator(Session, Self->Auth, Self->Owner->FUI, Self->PasswordUsed).Authenticate();
    });
  }
};

// Connection > SSH > Authentication > Allow agent forwarding (AgentFwd): the
// server opens "auth-agent@openssh.com" channels, which are relayed to the
// local ssh-agent (SSH_AUTH_SOCK). Agent messages are short request/response
// pairs, relayed synchronously from libssh's data callback; the callbacks run
// whenever the worker processes SSH packets (reading any channel).
struct SecureShell::AgentForwarding
{
  struct Forward
  {
    ssh_channel Channel = nullptr;
    Net::Socket Socket = Net::Invalid;
    QByteArray Buffer;
    bool Closed = false;
    ssh_channel_callbacks_struct Callbacks{};
  };

  ssh_session Session;
  ssh_callbacks_struct SessionCallbacks{};
  std::list<std::unique_ptr<Forward>> Forwards;
  int Opened = 0;

  explicit AgentForwarding(ssh_session ASession) :
    Session(ASession)
  {
    ssh_callbacks_init(&SessionCallbacks);
    SessionCallbacks.userdata = this;
    SessionCallbacks.channel_open_request_auth_agent_function = &OpenRequest;
    ssh_set_callbacks(Session, &SessionCallbacks);
  }

  ~AgentForwarding()
  {
    // The session outlives this object: do not leave it pointing to the
    // freed SessionCallbacks (libssh has no way to unset the callbacks)
    static ssh_callbacks_struct NoCallbacks = []
    {
      ssh_callbacks_struct Result{};
      ssh_callbacks_init(&Result);
      return Result;
    }();
    ssh_set_callbacks(Session, &NoCallbacks);
    for (auto & F : Forwards)
    {
      Release(*F);
    }
  }

  static void Release(Forward & F)
  {
    if (F.Socket != Net::Invalid)
    {
      Net::Close(F.Socket);
      F.Socket = Net::Invalid;
    }
    if (F.Channel != nullptr)
    {
      ssh_remove_channel_callbacks(F.Channel, &F.Callbacks);
      // sends the channel close, if not done yet
      ssh_channel_free(F.Channel);
      F.Channel = nullptr;
    }
  }

  // Frees finished channels (never from within libssh callbacks)
  void Cleanup()
  {
    for (auto I = Forwards.begin(); I != Forwards.end();)
    {
      if ((*I)->Closed)
      {
        Release(**I);
        I = Forwards.erase(I);
      }
      else
      {
        ++I;
      }
    }
  }

  static ssh_channel OpenRequest(ssh_session Session, void * UserData)
  {
    AgentForwarding * Self = static_cast<AgentForwarding *>(UserData);
#ifdef Q_OS_WIN
    // The Windows agents (Pageant, OpenSSH's named pipe) are not supported yet
    Q_UNUSED(Session);
    Q_UNUSED(Self);
    return nullptr;
#else
    QByteArray Path = AgentSocketPath();
    sockaddr_un Address{};
    if (Path.isEmpty() || (static_cast<size_t>(Path.size()) >= sizeof(Address.sun_path)))
    {
      return nullptr;
    }
    Net::Socket Socket = Net::Open(AF_UNIX, SOCK_STREAM, 0);
    if (Socket == Net::Invalid)
    {
      return nullptr;
    }
    Address.sun_family = AF_UNIX;
    memcpy(Address.sun_path, Path.constData(), static_cast<size_t>(Path.size()));
    if (::connect(Socket, reinterpret_cast<const sockaddr *>(&Address), sizeof(Address)) != 0)
    {
      Net::Close(Socket);
      return nullptr;
    }
    ssh_channel Channel = ssh_channel_new(Session);
    if (Channel == nullptr)
    {
      Net::Close(Socket);
      return nullptr;
    }
    auto F = std::make_unique<Forward>();
    F->Channel = Channel;
    F->Socket = Socket;
    ssh_callbacks_init(&F->Callbacks);
    F->Callbacks.userdata = F.get();
    F->Callbacks.channel_data_function = &ChannelData;
    F->Callbacks.channel_eof_function = &ChannelEof;
    F->Callbacks.channel_close_function = &ChannelClose;
    ssh_add_channel_callbacks(Channel, &F->Callbacks);
    Self->Forwards.push_back(std::move(F));
    Self->Opened++;
    return Channel;
#endif
  }

  // One agent request -> response over the agent socket
  static QByteArray Relay(Net::Socket Socket, const QByteArray & Request)
  {
    try
    {
      // Generous timeout: the agent may ask for a confirmation (ssh-add -c)
      ProxyIo Io(Socket, 60);
      Io.Send(Request);
      QByteArray Head = Io.Receive(4);
      quint32 Len = (static_cast<quint32>(static_cast<unsigned char>(Head[0])) << 24) |
        (static_cast<quint32>(static_cast<unsigned char>(Head[1])) << 16) |
        (static_cast<quint32>(static_cast<unsigned char>(Head[2])) << 8) |
        static_cast<quint32>(static_cast<unsigned char>(Head[3]));
      if (Len > 256 * 1024)
      {
        return QByteArray();
      }
      return Head + Io.Receive(Len);
    }
    catch (...)
    {
      return QByteArray();
    }
  }

  static int ChannelData(ssh_session, ssh_channel Channel, void * Data, uint32_t Len, int IsStderr, void * UserData)
  {
    Forward * F = static_cast<Forward *>(UserData);
    if (IsStderr || F->Closed)
    {
      return static_cast<int>(Len);
    }
    F->Buffer.append(static_cast<const char *>(Data), static_cast<qsizetype>(Len));
    while (F->Buffer.size() >= 4)
    {
      const unsigned char * B = reinterpret_cast<const unsigned char *>(F->Buffer.constData());
      quint32 MessageLen = (static_cast<quint32>(B[0]) << 24) | (static_cast<quint32>(B[1]) << 16) |
        (static_cast<quint32>(B[2]) << 8) | static_cast<quint32>(B[3]);
      if (MessageLen > 256 * 1024)
      {
        F->Closed = true;
        break;
      }
      if (F->Buffer.size() < static_cast<qsizetype>(MessageLen) + 4)
      {
        break;
      }
      QByteArray Request = F->Buffer.left(static_cast<qsizetype>(MessageLen) + 4);
      F->Buffer.remove(0, static_cast<qsizetype>(MessageLen) + 4);
      QByteArray Response = Relay(F->Socket, Request);
      if (Response.isEmpty())
      {
        // SSH_AGENT_FAILURE
        Response = QByteArray("\0\0\0\x01\x05", 5);
      }
      ssh_channel_write(Channel, Response.constData(), static_cast<uint32_t>(Response.size()));
    }
    return static_cast<int>(Len);
  }

  static void ChannelEof(ssh_session, ssh_channel, void * UserData)
  {
    static_cast<Forward *>(UserData)->Closed = true;
  }

  static void ChannelClose(ssh_session, ssh_channel, void * UserData)
  {
    static_cast<Forward *>(UserData)->Closed = true;
  }
};

SecureShell::SecureShell(const SessionData & Data, UserInterface * UI) :
  FData(Data),
  FUI(UI)
{
}

SecureShell::~SecureShell()
{
  Close();
}

[[noreturn]] void SecureShell::SshError(const QString & Message)
{
  QString More = SshErrorText(FSession);
  if ((FSession == nullptr) || !ssh_is_connected(FSession))
  {
    throw FatalException(Message, More);
  }
  throw ExtException(Message, More);
}

void SecureShell::CheckConnection()
{
  if (!Active())
  {
    throw FatalException(QStringLiteral("Connection has been unexpectedly closed."), SshErrorText(FSession));
  }
}

bool SecureShell::Active() const
{
  return (FSession != nullptr) && ssh_is_connected(FSession);
}

QStringList SecureShell::SessionInfo() const
{
  QStringList Lines;
  if (FSession == nullptr)
  {
    return Lines;
  }
  // TSessionInfo / TFileSystemInfo (Server and protocol information dialog)
  Lines << QStringLiteral("Server: %1").arg(QString::fromUtf8(ssh_get_serverbanner(FSession)));
  Lines << QStringLiteral("SSH protocol version: %1").arg(ssh_get_version(FSession));
  auto PerDirection = [](const char * Out, const char * In) -> QString
  {
    QString CS = QString::fromUtf8((Out != nullptr) ? Out : "");
    QString SC = QString::fromUtf8((In != nullptr) ? In : "");
    if ((CS == SC) || SC.isEmpty())
    {
      return CS;
    }
    return QStringLiteral("%1 (client-to-server), %2 (server-to-client)").arg(CS, SC);
  };
  QString Cipher = PerDirection(ssh_get_cipher_out(FSession), ssh_get_cipher_in(FSession));
  if (!Cipher.isEmpty()) Lines << QStringLiteral("Encryption algorithm: %1").arg(Cipher);
  QString Hmac = PerDirection(ssh_get_hmac_out(FSession), ssh_get_hmac_in(FSession));
  if (!Hmac.isEmpty()) Lines << QStringLiteral("MAC algorithm: %1").arg(Hmac);
  if (const char * Kex = ssh_get_kex_algo(FSession)) Lines << QStringLiteral("Key exchange algorithm: %1").arg(QString::fromUtf8(Kex));
  // libssh does not report the negotiated compression; it follows the setting
  Lines << QStringLiteral("Compression: %1").arg(FData.Compression ? QStringLiteral("Yes") : QStringLiteral("No"));
  if (!FHostKey.KeyType.isEmpty())
  {
    Lines << QStringLiteral("Host key algorithm: %1").arg(FHostKey.KeyType);
    Lines << QStringLiteral("Host key fingerprint (SHA-256): %1").arg(FHostKey.SHA256);
    Lines << QStringLiteral("Host key fingerprint (MD5): %1").arg(FHostKey.MD5);
  }
  return Lines;
}

void SecureShell::Open()
{
  Connect();
  try
  {
    FHostKey = ServerHostKey(FSession);
    SessionLog::LogEvent(QStringLiteral("Server version: %1\nHost key fingerprint is:\n%2 %3").arg(QString::fromUtf8(ssh_get_serverbanner(FSession)), FHostKey.KeyType, FHostKey.SHA256));
    CheckWeakAlgorithms(FSession, FData, FHostKey.KeyType, FUI);
    VerifyHostKey();
    Authenticate();
    SessionLog::LogEvent(QStringLiteral("Access granted\n") + SessionInfo().join(QLatin1Char('\n')));
  }
  catch (...)
  {
    Close();
    throw;
  }
  StartKeepAlive();
}

SshHostKeyInfo SecureShell::ScanHostKey()
{
  Connect();
  try
  {
    FHostKey = ServerHostKey(FSession);
  }
  catch (...)
  {
    Close();
    throw;
  }
  Close();
  return FHostKey;
}

void SecureShell::Close()
{
  delete FPingTimer;
  FPingTimer = nullptr;
  if (FSession != nullptr)
  {
    // Forwarded agent channels belong to the session (ssh_disconnect frees
    // them); the destructor also detaches the session callbacks
    FAgent.reset();
    if (ssh_is_connected(FSession))
    {
      ssh_disconnect(FSession);
    }
    ssh_free(FSession);
    FSession = nullptr;
  }
  FAgent.reset();
  FJump.reset();
}

QString SecureShell::CipherOption(const SessionData & Data)
{
  return AlgorithmOption(Data.Ciphers(), SessionData::DefaultCipherList().split(QLatin1Char(',')), CipherAlgorithms, false);
}

QString SecureShell::KexOption(const SessionData & Data)
{
  return AlgorithmOption(Data.Kexes(), SessionData::DefaultKexList().split(QLatin1Char(',')), KexAlgorithms, false);
}

QString SecureShell::HostKeyOption(const SessionData & Data)
{
  // With the default list libssh's own order is kept: it prefers the key
  // types already in known_hosts, which avoids false "host key changed" alarms
  return AlgorithmOption(Data.HostKeys(), SessionData::DefaultHostKeyList().split(QLatin1Char(',')), HostKeyAlgorithms, true);
}

QString SecureShell::ProxyCommandOption(const SessionData & Data)
{
  // Only "Local" proxies run a command; SOCKS, HTTP and Telnet proxies are
  // negotiated internally (OpenProxySocket)
  if ((Data.ProxyMethod != pmCmd) || !UsesProxy(Data))
  {
    return QString();
  }
  return FormatProxyCommand(Data.ProxyLocalCommand, Data, Data.HostName);
}

bool SecureShell::PromptProxyCredentials()
{
  // PROXY_AUTH_TITLE, PROXY_AUTH_USERNAME_PROMPT, PROXY_AUTH_PASSWORD_PROMPT
  QList<Prompt> Prompts{
    Prompt{QStringLiteral("Proxy &username:"), true, FData.ProxyUsername},
    Prompt{QStringLiteral("Proxy &password:"), false, QString()}};
  if (!FUI->PromptUser(UserInterface::pkPassword, QStringLiteral("Proxy authentication"), QString(), Prompts))
  {
    return false;
  }
  FData.ProxyUsername = Prompts[0].Result;
  FData.ProxyPassword = Prompts[1].Result;
  return true;
}

Net::Socket SecureShell::OpenProxySocket()
{
  bool AtProxy = DnsAtProxy(FData);
  for (int Attempt = 0;; Attempt++)
  {
    Net::Socket Socket = ConnectTcp(FData.ProxyHost, FData.ProxyPort, FData.AddressFamily, FData.SourceAddress, FData.Timeout);
    try
    {
      ProxyIo Io(Socket, FData.Timeout);
      switch (FData.ProxyMethod)
      {
        case pmSocks4:
          NegotiateSocks4(Io, FData, AtProxy);
          break;
        case pmSocks5:
          NegotiateSocks5(Io, FData, AtProxy);
          break;
        case pmHTTP:
        {
          QString Status;
          if (!NegotiateHttp(Io, FData, AtProxy, Status))
          {
            // 407: ask for the credentials once, when none are configured
            Net::Close(Socket);
            Socket = Net::Invalid;
            bool CanPrompt = (Attempt == 0) && FData.ProxyUsername.isEmpty();
            if (CanPrompt && !PromptProxyCredentials())
            {
              throw AbortException();
            }
            if (!CanPrompt)
            {
              throw FatalException(QStringLiteral("Proxy authentication failed."), Status);
            }
            continue;
          }
          break;
        }
        case pmTelnet:
        {
          QString Target = FData.HostName;
          int Family = AF_UNSPEC;
          LiteralAddress(Target, Family);
          if ((Family == AF_UNSPEC) && !AtProxy)
          {
            QByteArray Address = ResolveAddress(Target, FData.PortNumber, AddressFamilyToAF(FData.AddressFamily), Family);
            Target = AddressToString(Address, Family);
          }
          NegotiateTelnet(Io, FData, Target);
          break;
        }
        default:
          break;
      }
    }
    catch (...)
    {
      if (Socket != Net::Invalid)
      {
        Net::Close(Socket);
      }
      throw;
    }
    return Socket;
  }
}

void SecureShell::ApplyOptions()
{
  QByteArray Host = FData.HostName.toUtf8();
  unsigned int Port = static_cast<unsigned int>(FData.PortNumber);
  long Timeout = FData.Timeout;
  ssh_options_set(FSession, SSH_OPTIONS_HOST, Host.constData());
  ssh_options_set(FSession, SSH_OPTIONS_PORT, &Port);
  ssh_options_set(FSession, SSH_OPTIONS_TIMEOUT, &Timeout);
  FKnownHostName = FData.HostName;
  FKnownHostPort = FData.PortNumber;
  if (!FData.KnownHostsFile.isEmpty())
  {
    QByteArray KnownHosts = QFile::encodeName(FData.KnownHostsFile);
    ssh_options_set(FSession, SSH_OPTIONS_KNOWNHOSTS, KnownHosts.constData());
  }
  if (FData.Compression)
  {
    ssh_options_set(FSession, SSH_OPTIONS_COMPRESSION, "yes");
  }
  if (FData.TcpNoDelay || FForceNoDelay)
  {
    // Otherwise libssh's default is kept (it matters for the SFTP pipeline)
    int NoDelay = 1;
    ssh_options_set(FSession, SSH_OPTIONS_NODELAY, &NoDelay);
  }
  if (!FData.SourceAddress.isEmpty())
  {
    QByteArray Bind = FData.SourceAddress.toUtf8();
    ssh_options_set(FSession, SSH_OPTIONS_BINDADDR, Bind.constData());
  }
  // Authentication page: GSSAPI credential delegation (GSSAPIFwdTGT)
  int Delegate = (FData.AuthGSSAPI && FData.GSSAPIFwdTGT) ? 1 : 0;
  ssh_options_set(FSession, SSH_OPTIONS_GSSAPI_DELEGATE_CREDENTIALS, &Delegate);

  // SSH page: algorithm preferences
  auto SetAlgorithms = [this](ssh_options_e Option, ssh_options_e Option2, const QString & Value, const char * What)
  {
    if (Value.isEmpty())
    {
      return;
    }
    QByteArray Utf = Value.toUtf8();
    if ((ssh_options_set(FSession, Option, Utf.constData()) != SSH_OK) ||
        ((Option2 != Option) && (ssh_options_set(FSession, Option2, Utf.constData()) != SSH_OK)))
    {
      throw FatalException(QStringLiteral("Connection failed."),
        QStringLiteral("None of the configured %1 algorithms is supported.\n%2").arg(QString::fromLatin1(What), SshErrorText(FSession)));
    }
  };
  QString Ciphers = CipherOption(FData);
  SetAlgorithms(SSH_OPTIONS_CIPHERS_C_S, SSH_OPTIONS_CIPHERS_S_C, Ciphers, "encryption");
  SetAlgorithms(SSH_OPTIONS_KEY_EXCHANGE, SSH_OPTIONS_KEY_EXCHANGE, KexOption(FData), "key exchange");
  SetAlgorithms(SSH_OPTIONS_HOSTKEYS, SSH_OPTIONS_HOSTKEYS, HostKeyOption(FData), "host key");

  // Re-keying: RekeyBytes, unless the server "handles SSH key re-exchange
  // badly" (BugRekey2 forced on). RekeyTime is not applied: with
  // SSH_OPTIONS_REKEY_TIME set, libssh 0.11 delays every request by a
  // delayed-ACK round trip (measured: SFTP stat 0.1 ms -> 48 ms)
  uint64_t RekeyData = static_cast<uint64_t>(std::max<qint64>(FData.RekeyBytes(), 0));
  if ((RekeyData > 0) && (FData.Bug[sbRekey2] != asOn))
  {
    ssh_options_set(FSession, SSH_OPTIONS_REKEY_DATA, &RekeyData);
  }

  // Connection > Tunnel: libssh ProxyJump to the tunnel host (no local
  // forwarded port, so TunnelLocalPortNumber has nothing to control)
  if (FData.Tunnel && !FData.TunnelHostName.isEmpty())
  {
    // TSessionData::CreateTunnelData: the tunnel inherits the
    // authentication options (TryAgent, AuthKI, AuthKIPassword, AuthGSSAPI)
    // of the main session
    FJump = std::make_unique<JumpData>();
    FJump->Owner = this;
    FJump->HostName = FData.TunnelHostName;
    FJump->Port = FData.TunnelPortNumber;
    FJump->HostKey = FData.TunnelHostKey;
    FJump->Auth.UserName = FData.TunnelUserName;
    FJump->Auth.Password = FData.TunnelPassword;
    FJump->Auth.PublicKeyFile = FData.TunnelPublicKeyFile;
    FJump->Auth.TryAgent = FData.TryAgent;
    FJump->Auth.AuthKI = FData.AuthKI;
    FJump->Auth.AuthKIPassword = FData.AuthKIPassword;
    FJump->Auth.AuthGSSAPI = FData.AuthGSSAPI;
    FJump->Auth.SessionName = FData.TunnelUserName.isEmpty() ? FData.TunnelHostName :
      QStringLiteral("%1@%2").arg(FData.TunnelUserName, FData.TunnelHostName);
    QString Jump = FData.TunnelHostName;
    if (Jump.contains(QLatin1Char(':')))
    {
      Jump = QStringLiteral("[%1]").arg(Jump);
    }
    if (!FData.TunnelUserName.isEmpty())
    {
      Jump = FData.TunnelUserName + QLatin1Char('@') + Jump;
    }
    Jump += QStringLiteral(":%1").arg(FData.TunnelPortNumber);
    QByteArray JumpUtf = Jump.toUtf8();
    FJump->Callbacks.userdata = FJump.get();
    FJump->Callbacks.before_connection = &JumpData::BeforeConnection;
    FJump->Callbacks.verify_knownhost = &JumpData::VerifyKnownHost;
    FJump->Callbacks.authenticate = &JumpData::Authenticate;
    if ((ssh_options_set(FSession, SSH_OPTIONS_PROXYJUMP, JumpUtf.constData()) != SSH_OK) ||
        (ssh_options_set(FSession, SSH_OPTIONS_PROXYJUMP_CB_LIST_APPEND, &FJump->Callbacks) != SSH_OK))
    {
      throw FatalException(QStringLiteral("Error while tunneling the connection.\n \n%1").arg(SshErrorText(FSession)));
    }
    return;
  }

  // Connection > Proxy
  bool Proxied = UsesProxy(FData);
  if (Proxied && (FData.ProxyMethod == pmCmd))
  {
    QByteArray Utf = FormatProxyCommand(FData.ProxyLocalCommand, FData, FData.HostName).toUtf8();
    ssh_options_set(FSession, SSH_OPTIONS_PROXYCOMMAND, Utf.constData());
    return;
  }
  if (!Proxied && (FData.AddressFamily == afAuto) && FData.LogicalHostName.isEmpty())
  {
    // libssh connects itself
    return;
  }
  // Own socket: SOCKS/HTTP/Telnet proxy negotiated here, the address family
  // (libssh has no option for it) or a logical host name (the host key is
  // then looked up under that name, while the real host is connected)
  Net::Socket Socket = Proxied ? OpenProxySocket() :
    ConnectTcp(FData.HostName, FData.PortNumber, FData.AddressFamily, FData.SourceAddress, FData.Timeout);
  socket_t Fd = Socket;
  ssh_options_set(FSession, SSH_OPTIONS_FD, &Fd);
  if (!FData.LogicalHostName.isEmpty())
  {
    // LogicalHostName ("name" or "name:port", PuTTY's loghost)
    QString Name = FData.LogicalHostName.trimmed();
    int LogicalPort = FData.PortNumber;
    qsizetype Colon = Name.lastIndexOf(QLatin1Char(':'));
    if ((Colon > 0) && (Name.indexOf(QLatin1Char(':')) == Colon))
    {
      bool Ok = false;
      int P = Name.mid(Colon + 1).toInt(&Ok);
      if (Ok && (P > 0))
      {
        LogicalPort = P;
      }
      Name = Name.left(Colon);
    }
    FKnownHostName = Name;
    FKnownHostPort = LogicalPort;
    QByteArray NameUtf = Name.toUtf8();
    unsigned int PortValue = static_cast<unsigned int>(LogicalPort);
    ssh_options_set(FSession, SSH_OPTIONS_HOST, NameUtf.constData());
    ssh_options_set(FSession, SSH_OPTIONS_PORT, &PortValue);
  }
}

void SecureShell::Connect()
{
  FSession = ssh_new();
  if (FSession == nullptr)
  {
    throw FatalException(QStringLiteral("Connection failed."));
  }
  SessionLog::SetupSsh(FSession);
  if (FData.UserName.isEmpty())
  {
    QList<Prompt> Prompts{Prompt{QStringLiteral("&Username:"), true, QString()}};
    if (!FUI->PromptUser(UserInterface::pkUserName, FData.SessionName(), QString(), Prompts))
    {
      ssh_free(FSession);
      FSession = nullptr;
      throw AbortException();
    }
    FData.UserName = Prompts[0].Result;
  }
  try
  {
    ApplyOptions();
  }
  catch (...)
  {
    ssh_free(FSession);
    FSession = nullptr;
    FJump.reset();
    throw;
  }
  QByteArray User = FData.UserName.toUtf8();
  ssh_options_set(FSession, SSH_OPTIONS_USER, User.constData());

  SessionLog::LogEvent(QStringLiteral("Looking up host \"%1\" for SSH connection").arg(FData.HostName));
  SessionLog::LogEvent(QStringLiteral("Connecting to %1 port %2").arg(FData.HostName).arg(FData.PortNumber));
  if (ssh_connect(FSession) != SSH_OK)
  {
    QString More = SshErrorText(FSession);
    ssh_free(FSession);
    FSession = nullptr;
    std::unique_ptr<JumpData> Jump = std::move(FJump);
    if (Jump && Jump->Aborted)
    {
      throw AbortException();
    }
    if (Jump && !Jump->Error.isEmpty())
    {
      throw FatalException(Jump->Error, Jump->MoreMessages);
    }
    ConnectionError(FData.HostName, More);
  }
}

void SecureShell::VerifyHostKey()
{
  VerifySessionHostKey(FSession, FKnownHostName, FKnownHostPort, FData.HostKey, FData.SessionName(), FUI, &FHostKey);
}

void SecureShell::Authenticate()
{
  AuthSettings Settings;
  Settings.SessionName = FData.SessionName();
  Settings.UserName = FData.UserName;
  Settings.Password = FData.Password;
  Settings.PublicKeyFile = FData.PublicKeyFile;
  Settings.DetachedCertificate = FData.DetachedCertificate;
  Settings.TryAgent = FData.TryAgent;
  Settings.AuthKI = FData.AuthKI;
  Settings.AuthKIPassword = FData.AuthKIPassword;
  Settings.AuthGSSAPI = FData.AuthGSSAPI;
  Settings.NoUserAuth = FData.SshNoUserAuth;
  // Password change on expiry (PuTTY's SSH_MSG_USERAUTH_PASSWD_CHANGEREQ /
  // pkNewPassword) is not available: libssh has no API for it. Servers
  // usually handle expired passwords through keyboard-interactive, which is
  // fully supported (the "New password" prompts are shown as they come).
  SessionLog::LogEvent(QStringLiteral("Using username \"%1\".").arg(FData.UserName));
  Authenticator(FSession, Settings, FUI, FPasswordUsed).Authenticate();
}

void SecureShell::StartKeepAlive()
{
  if ((FData.PingType == ptOff) || (FData.PingInterval <= 0))
  {
    return;
  }
  FPingTimer = new QTimer();
  FPingTimer->setInterval(FData.PingInterval * 1000);
  QObject::connect(FPingTimer, &QTimer::timeout, FPingTimer, [this]()
  {
    try
    {
      if ((FData.PingType == ptDummyCommand) && FDummyCommand)
      {
        FDummyCommand();
      }
      else
      {
        KeepAlive();
      }
    }
    catch (...)
    {
      // a lost connection is reported by the next operation
    }
  });
  FPingTimer->start();
}

void SecureShell::KeepAlive()
{
  if (Active())
  {
    if (FAgent)
    {
      FAgent->Cleanup();
    }
    if (FData.Bug[sbIgnore2] == asOn)
    {
      // The server "chokes on SSH ignore messages": SSH_MSG_DEBUG instead
      // (libssh offers no keepalive@openssh.com global request)
      ssh_send_debug(FSession, "", 0);
    }
    else
    {
      // TSecureShell::KeepAlive: SSH_MSG_IGNORE
      ssh_send_ignore(FSession, "");
    }
  }
}

void SecureShell::RequestAgentForwarding(ssh_channel Channel)
{
  if (!FData.AgentFwd || (Channel == nullptr) || !Active() || AgentSocketPath().isEmpty())
  {
    return;
  }
  if (!FAgent)
  {
    FAgent = std::make_unique<AgentForwarding>(FSession);
  }
  else
  {
    FAgent->Cleanup();
  }
  // A refusal is not an error (WinSCP only logs it)
  ssh_channel_request_auth_agent(Channel);
}

int SecureShell::AgentChannelsOpened() const
{
  return FAgent ? FAgent->Opened : 0;
}

QString SecureShell::MatchingCertificateFile(const QString & KeyFileName)
{
  // AddMatchingKeyCertificate: "<key>-cert.pub" or "<key>.pub-aadcert.pub",
  // with a ".ppk" extension removed first
  QString Base = ExpandHome(KeyFileName);
  if (Base.isEmpty())
  {
    return QString();
  }
  if (Base.endsWith(QStringLiteral(".ppk"), Qt::CaseInsensitive))
  {
    Base.chop(4);
  }
  for (const QString & Suffix : {QStringLiteral("-cert.pub"), QStringLiteral(".pub-aadcert.pub")})
  {
    if (QFileInfo::exists(Base + Suffix))
    {
      return Base + Suffix;
    }
  }
  return QString();
}

QString SecureShell::PublicKeyLine(const QString & KeyFileName, QString * Comment)
{
  // GetPublicKeyLine: "<type> <base64 blob> <comment>" of the key pair
  QString FileName = ExpandHome(KeyFileName);
  QFile File(FileName);
  if (!File.open(QIODevice::ReadOnly))
  {
    // KEY_TYPE_UNOPENABLE
    throw ExtException(QStringLiteral("Private key file '%1' does not exist or cannot be opened.").arg(FileName), File.errorString());
  }
  QByteArray Content = File.read(1024 * 1024);
  File.close();
  QString Type;
  QString Blob;
  QString KeyComment;
  if (Content.startsWith("PuTTY-User-Key-File-"))
  {
    // The public part of a .ppk is never encrypted
    QList<QByteArray> Lines = Content.split('\n');
    for (qsizetype Index = 0; Index < Lines.size(); Index++)
    {
      QByteArray Line = Lines[Index].trimmed();
      qsizetype Colon = Line.indexOf(':');
      if (Colon < 0)
      {
        continue;
      }
      QByteArray Key = Line.left(Colon);
      QString Value = QString::fromUtf8(Line.mid(Colon + 1)).trimmed();
      if (Key.startsWith("PuTTY-User-Key-File-"))
      {
        Type = Value;
      }
      else if (Key == "Comment")
      {
        KeyComment = Value;
      }
      else if (Key == "Public-Lines")
      {
        int Count = Value.toInt();
        for (int I = 1; (I <= Count) && (Index + I < Lines.size()); I++)
        {
          Blob += QString::fromLatin1(Lines[Index + I].trimmed());
        }
        Index += Count;
      }
    }
  }
  else
  {
    // OpenSSH: the .pub file next to the key, or the public key embedded in
    // the (possibly encrypted) private key file
    QString PublicFile = FileName.endsWith(QStringLiteral(".pub")) ? FileName : FileName + QStringLiteral(".pub");
    QFile Pub(PublicFile);
    if (Pub.open(QIODevice::ReadOnly))
    {
      QStringList Parts = QString::fromUtf8(Pub.readLine(64 * 1024)).trimmed().split(QLatin1Char(' '), Qt::SkipEmptyParts);
      if (Parts.size() >= 2)
      {
        Type = Parts[0];
        Blob = Parts[1];
        KeyComment = Parts.mid(2).join(QLatin1Char(' '));
      }
    }
    if (Blob.isEmpty())
    {
      ssh_key Key = nullptr;
      QByteArray Path = QFile::encodeName(FileName);
      if (ssh_pki_import_pubkey_file(Path.constData(), &Key) == SSH_OK)
      {
        char * Base64 = nullptr;
        if (ssh_pki_export_pubkey_base64(Key, &Base64) == SSH_OK)
        {
          Blob = QString::fromLatin1(Base64);
          ssh_string_free_char(Base64);
        }
        Type = QString::fromUtf8(ssh_key_type_to_char(ssh_key_type(Key)));
        ssh_key_free(Key);
      }
    }
  }
  if (Type.isEmpty() || Blob.isEmpty())
  {
    // KEY_TYPE_UNKNOWN2
    throw ExtException(QStringLiteral("File '%1' does not contain private key in known format.").arg(FileName));
  }
  if (KeyComment.isEmpty())
  {
    KeyComment = QFileInfo(FileName).fileName();
  }
  if (Comment != nullptr)
  {
    *Comment = KeyComment;
  }
  return QStringLiteral("%1 %2 %3").arg(Type, Blob, KeyComment);
}

QString SecureShell::InstallPublicKey(const QString & KeyFileName, const std::function<void(const QString &)> & Information)
{
  // TTerminal::UploadPublicKey, over an SFTP channel of this connection
  auto Inform = [&](const QString & Message)
  {
    if (Information)
    {
      Information(Message);
    }
  };
  static const QString AuthorizedKeysFileName = QStringLiteral("authorized_keys");
  static const QString AuthorizedKeysFilePath = QStringLiteral(".ssh/authorized_keys");
  CheckConnection();
  QString Comment;
  QString Line = PublicKeyLine(KeyFileName, &Comment);
  Inform(QStringLiteral("Adding public key line to \"%1\" file:").arg(AuthorizedKeysFilePath) + QLatin1Char('\n') + Line);

  sftp_session Sftp = sftp_new(FSession);
  if ((Sftp == nullptr) || (sftp_init(Sftp) != SSH_OK))
  {
    QString More = SshErrorText(FSession);
    if (Sftp != nullptr)
    {
      sftp_free(Sftp);
    }
    CheckConnection();
    throw ExtException(QStringLiteral("Cannot initialize SFTP protocol. Is the host running an SFTP server?"), More);
  }
  std::unique_ptr<sftp_session_struct, void (*)(sftp_session)> SftpGuard(Sftp, sftp_free);
  auto SftpError = [&](const QString & Message) -> ExtException
  {
    return ExtException(Message, QStringLiteral("%1 (SFTP error %2)").arg(SshErrorText(FSession)).arg(sftp_get_error(Sftp)));
  };

  char * HomeRaw = sftp_canonicalize_path(Sftp, ".");
  if (HomeRaw == nullptr)
  {
    throw SftpError(QStringLiteral("Cannot get real path for '.'."));
  }
  QString Home = QString::fromUtf8(HomeRaw);
  ssh_string_free_char(HomeRaw);
  QString SshFolder = (Home.endsWith(QLatin1Char('/')) ? Home : Home + QLatin1Char('/')) + QStringLiteral(".ssh");
  QString AuthorizedKeysFile = SshFolder + QLatin1Char('/') + AuthorizedKeysFileName;

  // TTerminal::CheckRights: group/other writable entries are reported
  bool WrongRights = false;
  auto CheckRights = [&](const QString & Path) -> bool
  {
    QByteArray Utf = Path.toUtf8();
    sftp_attributes Attributes = sftp_stat(Sftp, Utf.constData());
    if (Attributes == nullptr)
    {
      return false;
    }
    if ((Attributes->permissions & 022) != 0)
    {
      WrongRights = true;
    }
    sftp_attributes_free(Attributes);
    return true;
  };

  if (!CheckRights(SshFolder))
  {
    QByteArray Utf = SshFolder.toUtf8();
    if (sftp_mkdir(Sftp, Utf.constData(), 0700) != SSH_OK)
    {
      throw SftpError(QStringLiteral("Error creating folder '%1'.").arg(SshFolder));
    }
  }

  QByteArray FileUtf = AuthorizedKeysFile.toUtf8();
  bool Exists = CheckRights(AuthorizedKeysFile);
  bool Updated = true;
  QByteArray Append;
  qint64 ExistingSize = 0;
  if (Exists)
  {
    Inform(QStringLiteral("Downloading current \"%1\" file...").arg(AuthorizedKeysFileName));
    sftp_file Read = sftp_open(Sftp, FileUtf.constData(), O_RDONLY, 0);
    if (Read == nullptr)
    {
      throw SftpError(QStringLiteral("Cannot open file '%1'.").arg(AuthorizedKeysFile));
    }
    QByteArray Content;
    char Buffer[16384];
    ssize_t N;
    while ((N = sftp_read(Read, Buffer, sizeof(Buffer))) > 0)
    {
      Content.append(Buffer, N);
    }
    sftp_close(Read);
    if (N < 0)
    {
      throw SftpError(QStringLiteral("Error reading file '%1'.").arg(AuthorizedKeysFile));
    }
    ExistingSize = Content.size();
    // Prefix: key type and blob, including the space after them
    QString Prefix = Line.section(QLatin1Char(' '), 0, 1) + QLatin1Char(' ');
    for (const QByteArray & Existing : Content.split('\n'))
    {
      QString ExistingLine = QString::fromUtf8(Existing).trimmed();
      if (ExistingLine.startsWith(Prefix) || (ExistingLine == Prefix.trimmed()))
      {
        Inform(QStringLiteral("\"%1\" file already contains public key line:").arg(AuthorizedKeysFileName) + QLatin1Char('\n') + ExistingLine);
        Updated = false;
      }
    }
    if (Updated)
    {
      Inform(QStringLiteral("\"%1\" file does not contain the public key line yet.").arg(AuthorizedKeysFileName));
      if (!Content.isEmpty() && !Content.endsWith('\n'))
      {
        Append += '\n';
      }
    }
  }
  else
  {
    Inform(QStringLiteral("Creating new \"%1\" file...").arg(AuthorizedKeysFileName));
  }

  if (Updated)
  {
    Append += Line.toUtf8() + '\n';
    Inform(QStringLiteral("Uploading updated \"%1\" file...").arg(AuthorizedKeysFileName));
    // The existing file is appended to, never rewritten, so a failure cannot
    // lose keys that are already there; a new file gets 0600
    sftp_file Write = Exists ?
      sftp_open(Sftp, FileUtf.constData(), O_WRONLY, 0) :
      sftp_open(Sftp, FileUtf.constData(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (Write == nullptr)
    {
      throw SftpError(QStringLiteral("Cannot create remote file '%1'.").arg(AuthorizedKeysFile));
    }
    bool Ok = (sftp_seek64(Write, static_cast<uint64_t>(ExistingSize)) == 0);
    qsizetype Done = 0;
    while (Ok && (Done < Append.size()))
    {
      ssize_t W = sftp_write(Write, Append.constData() + Done, static_cast<size_t>(Append.size() - Done));
      if (W <= 0)
      {
        Ok = false;
      }
      else
      {
        Done += W;
      }
    }
    sftp_close(Write);
    if (!Ok)
    {
      throw SftpError(QStringLiteral("Error writing to file '%1'.").arg(AuthorizedKeysFile));
    }
  }

  // PUBLIC_KEY_UPLOADED
  QString Result = QStringLiteral("**Public key \"%1\" was installed.**\n\nYou can now login to the server using the key pair.").arg(Comment);
  if (Updated)
  {
    Inform(QStringLiteral("Public key \"%1\" was installed.\nYou can now login to the server using the key pair.").arg(Comment));
  }
  if (WrongRights)
  {
    // PUBLIC_KEY_PERMISSIONS
    QString PermissionsInfo = QStringLiteral("Though potentially wrong permissions of \"%1\" file and/or its parent folder were detected. Please check them.").arg(AuthorizedKeysFilePath);
    Inform(PermissionsInfo);
    Result += QStringLiteral("\n\n") + PermissionsInfo;
  }
  return Result;
}

QString SecureShell::InstallPublicKeyToServer(const SessionData & Data, UserInterface * UI, const QString & KeyFileName,
  const std::function<void(const QString &)> & Information)
{
  SecureShell Shell(Data, UI);
  Shell.Open();
  return Shell.InstallPublicKey(KeyFileName, Information);
}

bool SecureShell::AutodetectProxy(TProxyMethod & Method, QString & Host, int & Port)
{
  // Proxy "Autodetect" button: the environment (all_proxy, https_proxy,
  // http_proxy) first, then Qt's system proxy configuration
  for (const char * Name : {"all_proxy", "ALL_PROXY", "https_proxy", "HTTPS_PROXY", "http_proxy", "HTTP_PROXY"})
  {
    QString Value = QString::fromLocal8Bit(qgetenv(Name)).trimmed();
    if (Value.isEmpty())
    {
      continue;
    }
    QUrl Url = QUrl::fromUserInput(Value.contains(QStringLiteral("://")) ? Value : QStringLiteral("http://") + Value);
    if (!Url.isValid() || Url.host().isEmpty())
    {
      continue;
    }
    QString Scheme = Url.scheme().toLower();
    if (Scheme.startsWith(QStringLiteral("socks4")))
    {
      Method = pmSocks4;
    }
    else if (Scheme.startsWith(QStringLiteral("socks")))
    {
      Method = pmSocks5;
    }
    else
    {
      Method = pmHTTP;
    }
    Host = Url.host();
    Port = Url.port((Method == pmHTTP) ? ProxyPortNumber : 1080);
    return true;
  }
  const QList<QNetworkProxy> Proxies =
    QNetworkProxyFactory::systemProxyForQuery(QNetworkProxyQuery(QUrl(QStringLiteral("https://winscp.net/"))));
  for (const QNetworkProxy & Proxy : Proxies)
  {
    if ((Proxy.type() == QNetworkProxy::HttpProxy) || (Proxy.type() == QNetworkProxy::Socks5Proxy))
    {
      Method = (Proxy.type() == QNetworkProxy::HttpProxy) ? pmHTTP : pmSocks5;
      Host = Proxy.hostName();
      Port = Proxy.port();
      return !Host.isEmpty();
    }
  }
  return false;
}

// ---------------------------------------------------------------- SshChannel

SshChannel::SshChannel(SecureShell & Shell) :
  FShell(Shell)
{
}

SshChannel::~SshChannel()
{
  Close();
}

void SshChannel::Open()
{
  Close();
  FShell.CheckConnection();
  FChannel = ssh_channel_new(FShell.Session());
  if ((FChannel == nullptr) || (ssh_channel_open_session(FChannel) != SSH_OK))
  {
    QString More = SshErrorText(FShell.Session());
    if (FChannel != nullptr)
    {
      ssh_channel_free(FChannel);
      FChannel = nullptr;
    }
    FShell.CheckConnection();
    throw ExtException(QStringLiteral("Server refused to open a session channel."), More);
  }
  FInput.clear();
  FInputPos = 0;
  FStdError.clear();
  FStdErrorLine.clear();
  // AgentFwd: requested on every session channel, before the shell/command
  FShell.RequestAgentForwarding(FChannel);
}

void SshChannel::OpenShell(const QString & Shell)
{
  Open();
  int Rc;
  if (Shell.isEmpty())
  {
    // No terminal is requested (PuTTY nopty), as WinSCP does for SCP
    Rc = ssh_channel_request_shell(FChannel);
  }
  else
  {
    QByteArray Utf = Encode(Shell);
    Rc = ssh_channel_request_exec(FChannel, Utf.constData());
  }
  if (Rc != SSH_OK)
  {
    QString More = SshErrorText(FShell.Session());
    Close();
    FShell.CheckConnection();
    throw FatalException(QStringLiteral("Server refused to start a shell/command."), More);
  }
}

void SshChannel::OpenExec(const QString & Command)
{
  Open();
  QByteArray Utf = Encode(Command);
  if (ssh_channel_request_exec(FChannel, Utf.constData()) != SSH_OK)
  {
    QString More = SshErrorText(FShell.Session());
    Close();
    FShell.CheckConnection();
    throw ExtException(QStringLiteral("Server refused to start a shell/command."), More);
  }
}

void SshChannel::Close()
{
  if (FChannel != nullptr)
  {
    if (FShell.Active() && ssh_channel_is_open(FChannel))
    {
      ssh_channel_close(FChannel);
    }
    ssh_channel_free(FChannel);
    FChannel = nullptr;
  }
  FInput.clear();
  FInputPos = 0;
}

bool SshChannel::IsOpen() const
{
  return (FChannel != nullptr) && FShell.Active() && ssh_channel_is_open(FChannel) && !ssh_channel_is_eof(FChannel);
}

[[noreturn]] void SshChannel::ChannelClosed()
{
  FShell.CheckConnection();
  QString More;
  if (FChannel != nullptr)
  {
    uint32_t ExitCode = 0;
    char * Signal = nullptr;
    int CoreDumped = 0;
    if ((ssh_channel_get_exit_state(FChannel, &ExitCode, &Signal, &CoreDumped) == SSH_OK))
    {
      if (Signal != nullptr)
      {
        More = QStringLiteral("Server sent command exit signal %1.").arg(QString::fromUtf8(Signal));
        ssh_string_free_char(Signal);
      }
      else
      {
        More = QStringLiteral("Server sent command exit status %1.").arg(ExitCode);
      }
    }
  }
  QString StdErr = StdError().trimmed();
  if (!StdErr.isEmpty())
  {
    More = StdErr + (More.isEmpty() ? QString() : QStringLiteral("\n") + More);
  }
  throw FatalException(QStringLiteral("Server unexpectedly closed network connection."), More);
}

void SshChannel::Send(const char * Data, qsizetype Len)
{
  if (FChannel == nullptr)
  {
    ChannelClosed();
  }
  while (Len > 0)
  {
    int Written = ssh_channel_write(FChannel, Data, static_cast<uint32_t>(std::min<qsizetype>(Len, 1024 * 1024)));
    if (Written == SSH_ERROR)
    {
      FShell.CheckConnection();
      ChannelClosed();
    }
    Data += Written;
    Len -= Written;
    if ((Written == 0) && !IsOpen())
    {
      ChannelClosed();
    }
  }
  // Keep stderr flowing, so the remote side never blocks on a full window
  DrainStdError();
}

void SshChannel::ConsumeInput(qsizetype Count)
{
  FInputPos += Count;
  if (FInputPos >= FInput.size())
  {
    FInput.clear();
    FInputPos = 0;
  }
}

QByteArray SshChannel::Encode(const QString & Str) const
{
  return FUtf ? Str.toUtf8() : Str.toLocal8Bit();
}

QString SshChannel::Decode(const QByteArray & Str) const
{
  return FUtf ? QString::fromUtf8(Str) : QString::fromLocal8Bit(Str);
}

void SshChannel::SendLine(const QString & Line)
{
  QByteArray Utf = Encode(Line);
  Utf.append('\n');
  Send(Utf);
}

void SshChannel::SendNull()
{
  char Null = 0;
  Send(&Null, 1);
}

void SshChannel::SendEof()
{
  if (FChannel != nullptr)
  {
    ssh_channel_send_eof(FChannel);
  }
}

void SshChannel::DrainStdError()
{
  if (FChannel == nullptr)
  {
    return;
  }
  char Buffer[4096];
  int Read;
  while ((Read = ssh_channel_read_nonblocking(FChannel, Buffer, sizeof(Buffer), 1)) > 0)
  {
    FStdError.append(Buffer, Read);
    if (FOnStdError)
    {
      FStdErrorLine.append(Buffer, Read);
      qsizetype P;
      while ((P = FStdErrorLine.indexOf('\n')) >= 0)
      {
        QByteArray Line = FStdErrorLine.left(P);
        FStdErrorLine.remove(0, P + 1);
        if (Line.endsWith('\r'))
        {
          Line.chop(1);
        }
        FOnStdError(QString::fromUtf8(Line));
      }
    }
  }
}

void SshChannel::WaitForStdError(int Ms)
{
  if (FChannel == nullptr)
  {
    return;
  }
  QElapsedTimer Timer;
  Timer.start();
  qint64 Remaining;
  while ((Remaining = Ms - Timer.elapsed()) > 0)
  {
    int Rc = ssh_channel_poll_timeout(FChannel, static_cast<int>(Remaining), 1);
    if (Rc <= 0)
    {
      break;
    }
    DrainStdError();
  }
}

void SshChannel::FlushStdErrorLine()
{
  DrainStdError();
  if (FOnStdError && !FStdErrorLine.isEmpty())
  {
    QByteArray Line = FStdErrorLine;
    FStdErrorLine.clear();
    FOnStdError(QString::fromUtf8(Line));
  }
}

bool SshChannel::Fill()
{
  if (FChannel == nullptr)
  {
    return false;
  }
  char Buffer[65536];
  // Consumed data is dropped here, not per line/block (perf: no memmove of
  // the whole buffer for every received line)
  if ((FInputPos > 0) && (FInputPos >= FInput.size() / 2))
  {
    FInput.remove(0, FInputPos);
    FInputPos = 0;
  }
  QElapsedTimer Idle;
  Idle.start();
  int Timeout = std::max(FShell.Data().Timeout, 1);
  for (;;)
  {
    if (FCancelProgress != nullptr)
    {
      FCancelProgress->CheckCancel();
    }
    int Read = ssh_channel_read_timeout(FChannel, Buffer, sizeof(Buffer), 0, 200);
    DrainStdError();
    if (Read > 0)
    {
      FInput.append(Buffer, Read);
      return true;
    }
    if (Read == SSH_ERROR)
    {
      FShell.CheckConnection();
      return false;
    }
    if (ssh_channel_is_eof(FChannel) || !ssh_channel_is_open(FChannel))
    {
      // stdout may still contain data that arrived together with EOF
      Read = ssh_channel_read_nonblocking(FChannel, Buffer, sizeof(Buffer), 0);
      if (Read > 0)
      {
        FInput.append(Buffer, Read);
        return true;
      }
      return false;
    }
    FShell.CheckConnection();
    if (Idle.elapsed() >= static_cast<qint64>(Timeout) * 1000)
    {
      // CONFIRM_PROLONG_TIMEOUT3 (TSecureShell::TimeoutPrompt)
      QueryUserParams Params;
      Params.Params = qpFatalAbort | qpAllowContinueOnError | qpIgnoreAbort;
      Params.HelpKeyword = QStringLiteral("message_host_is_not_communicating");
      Params.Type = qtConfirmation;
      unsigned int Answer = FShell.UI()->QueryUserEx(
        QStringLiteral("**Host is not communicating for %1 seconds.\n\nWait for another %1 seconds?**").arg(Timeout),
        QString(), qaRetry | qaAbort, Params);
      if (Answer != qaRetry)
      {
        throw FatalException(QStringLiteral("Connection failed."),
          QStringLiteral("Host is not communicating for %1 seconds.").arg(Timeout));
      }
      Idle.restart();
    }
  }
}

QString SshChannel::ReceiveLine()
{
  QString Line;
  if (!ReceiveLineOrEof(Line))
  {
    ChannelClosed();
  }
  return Line;
}

bool SshChannel::ReceiveLineOrEof(QString & Line)
{
  qsizetype P;
  while ((P = FInput.indexOf('\n', FInputPos)) < 0)
  {
    if (!Fill())
    {
      if (FInput.size() > FInputPos)
      {
        Line = Decode(FInput.mid(FInputPos));
        FInput.clear();
        FInputPos = 0;
        return true;
      }
      return false;
    }
  }
  QByteArray Raw = FInput.mid(FInputPos, P - FInputPos);
  ConsumeInput(P + 1 - FInputPos);
  if (Raw.endsWith('\r'))
  {
    Raw.chop(1);
  }
  Line = Decode(Raw);
  return true;
}

void SshChannel::Receive(char * Buffer, qsizetype Len)
{
  while (Len > 0)
  {
    if ((FInputPos >= FInput.size()) && !Fill())
    {
      ChannelClosed();
    }
    qsizetype Count = std::min(Len, FInput.size() - FInputPos);
    memcpy(Buffer, FInput.constData() + FInputPos, static_cast<size_t>(Count));
    ConsumeInput(Count);
    Buffer += Count;
    Len -= Count;
  }
}

int SshChannel::WaitForExitStatus()
{
  if (FChannel == nullptr)
  {
    return -1;
  }
  // Consume the rest of the output, then wait for the exit-status request
  while (Fill())
  {
    FInput.clear();
    FInputPos = 0;
  }
  uint32_t ExitCode = 0;
  char * Signal = nullptr;
  int CoreDumped = 0;
  QElapsedTimer Timer;
  Timer.start();
  for (;;)
  {
    int Rc = ssh_channel_get_exit_state(FChannel, &ExitCode, &Signal, &CoreDumped);
    if (Rc == SSH_OK)
    {
      break;
    }
    if ((Rc == SSH_ERROR) || !FShell.Active() || (Timer.elapsed() > 5000))
    {
      return -1;
    }
    // SSH_AGAIN: process incoming packets
    char Dummy[256];
    ssh_channel_read_timeout(FChannel, Dummy, sizeof(Dummy), 0, 100);
  }
  if (Signal != nullptr)
  {
    ssh_string_free_char(Signal);
    return -1;
  }
  return static_cast<int>(ExitCode);
}

// ---------------------------------------------------------------- helpers

QString ShellDelimitStr(const QString & Str)
{
  // DelimitStr(Str, '"'): backslash before $ \ ` "
  QString Result;
  Result.reserve(Str.size());
  for (QChar C : Str)
  {
    if ((C == QLatin1Char('$')) || (C == QLatin1Char('\\')) || (C == QLatin1Char('`')) || (C == QLatin1Char('"')))
    {
      Result += QLatin1Char('\\');
    }
    Result += C;
  }
  return Result;
}

QString ShellQuoteStr(const QString & Str)
{
  return QLatin1Char('"') + ShellDelimitStr(Str) + QLatin1Char('"');
}
