// Tests of core/PuttyKey (PuTTY .ppk loader, port of putty/sshpubk.c ppk_load_s).
//
// Fixtures in tests/data/ppk were made with puttygen 0.83: one key per type
// (<type>-v3.ppk, <type>.pub = puttygen -L), converted to PPK v2
// (--ppk-param version=2) and encrypted variants (-P, passphrase
// "linscp-test"; v3 with argon2id/argon2i/argon2d; ed25519-v3-utf8-enc.ppk
// uses a non-ASCII passphrase); rsa.openssh = puttygen -O private-openssh.
// Located via NIXSCP_TEST_DATA_DIR, falling back to the source tree path.
//
// When NIXSCP_TEST_SSH_PORT is set, the loaded keys are additionally used to
// authenticate against a real SSH server (whose authorized_keys must contain
// the tests/data/ppk/*.pub keys). Environment as in test_sftp:
//   NIXSCP_TEST_SSH_PORT, NIXSCP_TEST_SSH_HOST (default 127.0.0.1),
//   NIXSCP_TEST_SSH_USER (default $USER).
// The user's ~/.ssh/known_hosts is never touched.
#include <QFileInfo>
#include "core/PuttyKey.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cstdio>
#include <cstdlib>

static int Failures = 0;
static int Checks = 0;

#define CHECK(Cond) \
  do { Checks++; if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_MSG(Cond, Msg) \
  do { Checks++; if (!(Cond)) { std::printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, #Cond, qPrintable(Msg)); std::fflush(stdout); Failures++; } } while (0)

static const QString TestPassphrase = QStringLiteral("linscp-test");
static const QString TestUtf8Passphrase = QString::fromUtf8("p\xc3\xa4ssw\xc3\xb6rd");

static QString DataDir()
{
  QByteArray Env = qgetenv("NIXSCP_TEST_DATA_DIR");
  if (!Env.isEmpty())
  {
    return QString::fromLocal8Bit(Env);
  }
  return QFileInfo(QStringLiteral(__FILE__)).absolutePath() + QStringLiteral("/data/ppk");
}

static QString DataFile(const QString & Name)
{
  return DataDir() + QLatin1Char('/') + Name;
}

static QByteArray ReadFile(const QString & FileName)
{
  QFile File(FileName);
  if (!File.open(QIODevice::ReadOnly))
  {
    return QByteArray();
  }
  return File.readAll();
}

static void WriteFile(const QString & FileName, const QByteArray & Data)
{
  QFile File(FileName);
  if (File.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    File.write(Data);
  }
}

// "<type> <base64>" of a key, as in the first two fields of puttygen -L output
static QString PublicKeyString(ssh_key Key)
{
  char * Base64 = nullptr;
  if (ssh_pki_export_pubkey_base64(Key, &Base64) != SSH_OK)
  {
    return QString();
  }
  QString Result = QString::fromLatin1(ssh_key_type_to_char(ssh_key_type(Key))) + QLatin1Char(' ') + QString::fromLatin1(Base64);
  ssh_string_free_char(Base64);
  return Result;
}

static QString ExpectedPublicKey(const QString & Base)
{
  QList<QByteArray> Fields = ReadFile(DataFile(Base + QStringLiteral(".pub"))).trimmed().split(' ');
  if (Fields.size() < 2)
  {
    return QStringLiteral("<missing %1.pub>").arg(Base);
  }
  return QString::fromLatin1(Fields[0] + ' ' + Fields[1]);
}

// Passphrase prompt replaying a list of answers and recording the calls
struct PromptRecorder
{
  QStringList Answers; // exhausted => cancel
  QList<bool> RetryFlags;

  std::function<bool(QString &, bool)> Function()
  {
    return [this](QString & Passphrase, bool Retry) -> bool
    {
      RetryFlags.append(Retry);
      if (Answers.isEmpty())
      {
        return false;
      }
      Passphrase = Answers.takeFirst();
      return true;
    };
  }
};

static PuttyKey::LoadResult LoadKey(const QString & FileName, PromptRecorder & Prompt, ssh_key * Key, QString & Error)
{
  return PuttyKey::Load(FileName, Prompt.Function(), Key, &Error);
}

static void TestLoadMatches(const QString & File, const QString & Base, const QString & Passphrase, bool Encrypted)
{
  std::printf("  %s\n", qPrintable(File));
  PromptRecorder Prompt;
  Prompt.Answers << Passphrase;
  ssh_key Key = nullptr;
  QString Error;
  PuttyKey::LoadResult Result = LoadKey(DataFile(File), Prompt, &Key, Error);
  CHECK_MSG(Result == PuttyKey::lrOk, File + QStringLiteral(": ") + Error);
  CHECK_MSG(Prompt.RetryFlags.size() == (Encrypted ? 1 : 0), File);
  if (Encrypted && !Prompt.RetryFlags.isEmpty())
  {
    CHECK(!Prompt.RetryFlags[0]);
  }
  if (Key != nullptr)
  {
    CHECK_MSG(ssh_key_is_private(Key) == 1, File);
    CHECK_MSG(PublicKeyString(Key) == ExpectedPublicKey(Base), File);
    ssh_key_free(Key);
  }
}

static void TestAllFixtures()
{
  std::printf("Fixtures\n");
  const QStringList Bases{QStringLiteral("rsa"), QStringLiteral("ed25519"), QStringLiteral("ecdsa256"),
    QStringLiteral("ecdsa384"), QStringLiteral("ecdsa521")};
  for (const QString & Base : Bases)
  {
    TestLoadMatches(Base + QStringLiteral("-v2.ppk"), Base, QString(), false);
    TestLoadMatches(Base + QStringLiteral("-v3.ppk"), Base, QString(), false);
    TestLoadMatches(Base + QStringLiteral("-v2-enc.ppk"), Base, TestPassphrase, true);
    TestLoadMatches(Base + QStringLiteral("-v3-enc.ppk"), Base, TestPassphrase, true);
  }
  TestLoadMatches(QStringLiteral("ed25519-v3-argon2i-enc.ppk"), QStringLiteral("ed25519"), TestPassphrase, true);
  TestLoadMatches(QStringLiteral("ed25519-v3-argon2d-enc.ppk"), QStringLiteral("ed25519"), TestPassphrase, true);
  TestLoadMatches(QStringLiteral("ed25519-v3-utf8-enc.ppk"), QStringLiteral("ed25519"), TestUtf8Passphrase, true);
}

static void TestRsaMatchesOpenSsh()
{
  std::printf("RSA private key equals puttygen OpenSSH export\n");
  PromptRecorder Prompt;
  ssh_key Key = nullptr;
  QString Error;
  CHECK_MSG(LoadKey(DataFile(QStringLiteral("rsa-v3.ppk")), Prompt, &Key, Error) == PuttyKey::lrOk, Error);
  ssh_key OpenSshKey = nullptr;
  QByteArray Path = DataFile(QStringLiteral("rsa.openssh")).toLocal8Bit();
  CHECK(ssh_pki_import_privkey_file(Path.constData(), nullptr, nullptr, nullptr, &OpenSshKey) == SSH_OK);
  if ((Key != nullptr) && (OpenSshKey != nullptr))
  {
    CHECK(ssh_key_cmp(Key, OpenSshKey, SSH_KEY_CMP_PRIVATE) == 0);
  }
  ssh_key_free(Key);
  ssh_key_free(OpenSshKey);
}

static void TestDsa()
{
  std::printf("DSA\n");
  PromptRecorder Prompt;
  ssh_key Key = nullptr;
  QString Error;
  PuttyKey::LoadResult Result = LoadKey(DataFile(QStringLiteral("dsa-v3.ppk")), Prompt, &Key, Error);
  if (Result == PuttyKey::lrOk)
  {
    std::printf("  libssh supports DSA\n");
    CHECK(PublicKeyString(Key) == ExpectedPublicKey(QStringLiteral("dsa")));
    ssh_key_free(Key);
  }
  else
  {
    std::printf("  libssh without DSA: %s\n", qPrintable(Error));
    CHECK(Result == PuttyKey::lrError);
    CHECK_MSG(Error.contains(QStringLiteral("DSA")), Error);
    CHECK(Key == nullptr);
  }
}

static void TestPassphraseHandling()
{
  std::printf("Passphrase handling\n");
  const QStringList Files{QStringLiteral("rsa-v2-enc.ppk"), QStringLiteral("ed25519-v3-enc.ppk")};
  for (const QString & File : Files)
  {
    // Wrong, then right => ok after retry
    {
      PromptRecorder Prompt;
      Prompt.Answers << QStringLiteral("wrong") << TestPassphrase;
      ssh_key Key = nullptr;
      QString Error;
      CHECK_MSG(LoadKey(DataFile(File), Prompt, &Key, Error) == PuttyKey::lrOk, Error);
      CHECK(Prompt.RetryFlags == (QList<bool>{false, true}));
      CHECK(Key != nullptr);
      ssh_key_free(Key);
    }
    // Wrong three times => error, no fourth prompt
    {
      PromptRecorder Prompt;
      Prompt.Answers << QStringLiteral("a") << QStringLiteral("b") << QString() << TestPassphrase;
      ssh_key Key = reinterpret_cast<ssh_key>(1);
      QString Error;
      CHECK(LoadKey(DataFile(File), Prompt, &Key, Error) == PuttyKey::lrError);
      CHECK_MSG(Error == QStringLiteral("Wrong passphrase"), Error);
      CHECK(Prompt.RetryFlags == (QList<bool>{false, true, true}));
      CHECK(Key == nullptr);
    }
    // Cancel at first prompt
    {
      PromptRecorder Prompt;
      ssh_key Key = nullptr;
      QString Error;
      CHECK(LoadKey(DataFile(File), Prompt, &Key, Error) == PuttyKey::lrCancelled);
      CHECK(Prompt.RetryFlags.size() == 1);
      CHECK(Key == nullptr);
    }
    // Cancel at retry
    {
      PromptRecorder Prompt;
      Prompt.Answers << QStringLiteral("wrong");
      ssh_key Key = nullptr;
      QString Error;
      CHECK(LoadKey(DataFile(File), Prompt, &Key, Error) == PuttyKey::lrCancelled);
      CHECK(Prompt.RetryFlags == (QList<bool>{false, true}));
      CHECK(Key == nullptr);
    }
  }
  // Unencrypted key never prompts, even with a prompt that would cancel
  {
    PromptRecorder Prompt;
    ssh_key Key = nullptr;
    QString Error;
    CHECK(LoadKey(DataFile(QStringLiteral("ecdsa256-v3.ppk")), Prompt, &Key, Error) == PuttyKey::lrOk);
    CHECK(Prompt.RetryFlags.isEmpty());
    ssh_key_free(Key);
  }
  // Encrypted key without prompt callback
  {
    ssh_key Key = nullptr;
    QString Error;
    CHECK(PuttyKey::Load(DataFile(QStringLiteral("rsa-v3-enc.ppk")), nullptr, &Key, &Error) == PuttyKey::lrError);
    CHECK(Key == nullptr);
  }
}

// Replaces the value of the given header line
static QByteArray ReplaceHeader(const QByteArray & Data, const QByteArray & Name, const QByteArray & Value)
{
  QList<QByteArray> Lines = Data.split('\n');
  for (QByteArray & Line : Lines)
  {
    if (Line.startsWith(Name + ": "))
    {
      Line = Name + ": " + Value;
    }
  }
  return Lines.join('\n');
}

static QByteArray FlipHexChar(const QByteArray & Hex)
{
  QByteArray Result = Hex;
  Result[0] = (Result[0] == '0') ? '1' : '0';
  return Result;
}

static QByteArray HeaderValue(const QByteArray & Data, const QByteArray & Name)
{
  for (const QByteArray & Line : Data.split('\n'))
  {
    if (Line.startsWith(Name + ": "))
    {
      return Line.mid(Name.size() + 2).trimmed();
    }
  }
  return QByteArray();
}

static void TestCorruption()
{
  std::printf("Corrupted files\n");
  QTemporaryDir Dir;
  CHECK(Dir.isValid());
  const QStringList Plain{QStringLiteral("rsa-v2.ppk"), QStringLiteral("ed25519-v3.ppk"), QStringLiteral("ecdsa384-v3.ppk")};
  for (const QString & File : Plain)
  {
    QByteArray Data = ReadFile(DataFile(File));
    QString Target = Dir.filePath(File);

    // Corrupted MAC
    WriteFile(Target, ReplaceHeader(Data, "Private-MAC", FlipHexChar(HeaderValue(Data, "Private-MAC"))));
    PromptRecorder Prompt;
    ssh_key Key = nullptr;
    QString Error;
    CHECK(LoadKey(Target, Prompt, &Key, Error) == PuttyKey::lrError);
    CHECK_MSG(Error.contains(QStringLiteral("MAC")), Error);
    CHECK(Key == nullptr);
    CHECK(Prompt.RetryFlags.isEmpty());

    // Modified comment (covered by the MAC)
    WriteFile(Target, ReplaceHeader(Data, "Comment", "tampered"));
    CHECK(LoadKey(Target, Prompt, &Key, Error) == PuttyKey::lrError);
    CHECK_MSG(Error.contains(QStringLiteral("MAC")), Error);

    // Truncated file
    WriteFile(Target, Data.left(Data.indexOf("Private-Lines")));
    CHECK(LoadKey(Target, Prompt, &Key, Error) == PuttyKey::lrError);
    CHECK(Key == nullptr);

    // CRLF line ends are fine
    QByteArray Crlf = Data;
    Crlf.replace("\n", "\r\n");
    WriteFile(Target, Crlf);
    CHECK_MSG(LoadKey(Target, Prompt, &Key, Error) == PuttyKey::lrOk, Error);
    ssh_key_free(Key);
    Key = nullptr;
  }

  // Encrypted key with corrupted MAC is indistinguishable from wrong passphrase
  {
    QByteArray Data = ReadFile(DataFile(QStringLiteral("ecdsa256-v3-enc.ppk")));
    QString Target = Dir.filePath(QStringLiteral("enc.ppk"));
    WriteFile(Target, ReplaceHeader(Data, "Private-MAC", FlipHexChar(HeaderValue(Data, "Private-MAC"))));
    PromptRecorder Prompt;
    Prompt.Answers << TestPassphrase << TestPassphrase << TestPassphrase;
    ssh_key Key = nullptr;
    QString Error;
    CHECK(LoadKey(Target, Prompt, &Key, Error) == PuttyKey::lrError);
    CHECK_MSG(Error == QStringLiteral("Wrong passphrase"), Error);
    CHECK(Prompt.RetryFlags.size() == 3);

    // Absurd Argon2 memory is refused before deriving
    WriteFile(Target, ReplaceHeader(Data, "Argon2-Memory", "4000000000"));
    Prompt.Answers = QStringList{TestPassphrase};
    CHECK(LoadKey(Target, Prompt, &Key, Error) == PuttyKey::lrError);
    CHECK(Key == nullptr);
  }

  // Unsupported versions
  {
    QByteArray Data = ReadFile(DataFile(QStringLiteral("rsa-v3.ppk")));
    QString Target = Dir.filePath(QStringLiteral("v4.ppk"));
    QByteArray V4 = Data;
    V4.replace("PuTTY-User-Key-File-3", "PuTTY-User-Key-File-4");
    WriteFile(Target, V4);
    PromptRecorder Prompt;
    ssh_key Key = nullptr;
    QString Error;
    CHECK(LoadKey(Target, Prompt, &Key, Error) == PuttyKey::lrError);
    CHECK_MSG(Error.contains(QStringLiteral("too new")), Error);
    CHECK(PuttyKey::IsPuttyKeyFile(Target));
  }
}

static void TestIsPuttyKeyFile()
{
  std::printf("IsPuttyKeyFile\n");
  CHECK(PuttyKey::IsPuttyKeyFile(DataFile(QStringLiteral("rsa-v2.ppk"))));
  CHECK(PuttyKey::IsPuttyKeyFile(DataFile(QStringLiteral("ed25519-v3-enc.ppk"))));
  CHECK(!PuttyKey::IsPuttyKeyFile(DataFile(QStringLiteral("rsa.openssh"))));
  CHECK(!PuttyKey::IsPuttyKeyFile(DataFile(QStringLiteral("rsa.pub"))));
  CHECK(!PuttyKey::IsPuttyKeyFile(DataFile(QStringLiteral("does-not-exist.ppk"))));

  PromptRecorder Prompt;
  ssh_key Key = nullptr;
  QString Error;
  CHECK(LoadKey(DataFile(QStringLiteral("rsa.openssh")), Prompt, &Key, Error) == PuttyKey::lrError);
  CHECK(!Error.isEmpty());
  CHECK(LoadKey(DataFile(QStringLiteral("does-not-exist.ppk")), Prompt, &Key, Error) == PuttyKey::lrError);
  CHECK(Key == nullptr);
}

static void TestServerAuthentication()
{
  QByteArray PortStr = qgetenv("NIXSCP_TEST_SSH_PORT");
  if (PortStr.isEmpty())
  {
    std::printf("Server authentication: skipped (NIXSCP_TEST_SSH_PORT not set)\n");
    return;
  }
  std::printf("Server authentication\n");
  QByteArray Host = qgetenv("NIXSCP_TEST_SSH_HOST");
  if (Host.isEmpty())
  {
    Host = "127.0.0.1";
  }
  QByteArray User = qgetenv("NIXSCP_TEST_SSH_USER");
  if (User.isEmpty())
  {
    User = qgetenv("USER");
  }
  unsigned int Port = PortStr.toUInt();
  QTemporaryDir Dir;
  QByteArray KnownHosts = Dir.filePath(QStringLiteral("known_hosts")).toLocal8Bit();

  struct Case { const char * File; const char * Passphrase; };
  const Case Cases[] =
  {
    {"rsa-v2.ppk", nullptr},
    {"rsa-v3-enc.ppk", "linscp-test"},
    {"ed25519-v3.ppk", nullptr},
    {"ed25519-v2-enc.ppk", "linscp-test"},
    {"ecdsa256-v3-enc.ppk", "linscp-test"},
    {"ecdsa384-v2.ppk", nullptr},
    {"ecdsa521-v3.ppk", nullptr},
  };
  for (const Case & C : Cases)
  {
    QString File = QString::fromLatin1(C.File);
    std::printf("  %s\n", C.File);
    PromptRecorder Prompt;
    if (C.Passphrase != nullptr)
    {
      Prompt.Answers << QString::fromLatin1(C.Passphrase);
    }
    ssh_key Key = nullptr;
    QString Error;
    CHECK_MSG(LoadKey(DataFile(File), Prompt, &Key, Error) == PuttyKey::lrOk, Error);
    if (Key == nullptr)
    {
      continue;
    }
    ssh_session Session = ssh_new();
    ssh_options_set(Session, SSH_OPTIONS_HOST, Host.constData());
    ssh_options_set(Session, SSH_OPTIONS_PORT, &Port);
    ssh_options_set(Session, SSH_OPTIONS_USER, User.constData());
    ssh_options_set(Session, SSH_OPTIONS_KNOWNHOSTS, KnownHosts.constData());
    ssh_options_set(Session, SSH_OPTIONS_GLOBAL_KNOWNHOSTS, KnownHosts.constData());
    // Do not let the agent or ~/.ssh/config interfere
    int ProcessConfig = 0;
    ssh_options_set(Session, SSH_OPTIONS_PROCESS_CONFIG, &ProcessConfig);
    int Rc = ssh_connect(Session);
    CHECK_MSG(Rc == SSH_OK, QString::fromLatin1(ssh_get_error(Session)));
    if (Rc == SSH_OK)
    {
      // Host key is deliberately not verified: throwaway test server
      Rc = ssh_userauth_publickey(Session, nullptr, Key);
      CHECK_MSG(Rc == SSH_AUTH_SUCCESS, File + QStringLiteral(": ") + QString::fromLatin1(ssh_get_error(Session)));
      ssh_disconnect(Session);
    }
    ssh_free(Session);
    ssh_key_free(Key);
  }
}

int main()
{
  std::printf("Test data: %s\n", qPrintable(DataDir()));
  if (!QDir(DataDir()).exists())
  {
    std::printf("FAIL: test data directory not found (set NIXSCP_TEST_DATA_DIR)\n");
    return 1;
  }
  TestAllFixtures();
  TestRsaMatchesOpenSsh();
  TestDsa();
  TestPassphraseHandling();
  TestCorruption();
  TestIsPuttyKeyFile();
  TestServerAuthentication();
  std::printf("%d checks, %d failures\n", Checks, Failures);
  return (Failures == 0) ? 0 : 1;
}
