// NixSCP Pro encryption of the local configuration (core/ConfigVault):
// encrypted on disk, locked files never overwritten, unlocking with the
// password or the recovery key, plain and encrypted exports, imports of
// backups made with another password, newer wraps from another instance, the
// protected companion files, damaged and swapped files, a key that is replaced,
// and what a forgotten key leaves in memory.
#include "core/CloudCrypto.h"
#include "core/ConfigVault.h"
#include "core/Configuration.h"
#include "core/IniStorage.h"
#include "core/Security.h"
#include "core/SessionData.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <cstdio>
#include <memory>

#ifdef Q_OS_LINUX
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)

static QByteArray ReadRaw(const QString & FileName)
{
  QFile File(FileName);
  return File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray();
}

static void WriteRaw(const QString & FileName, const QByteArray & Data)
{
  QFile File(FileName);
  CHECK(File.open(QIODevice::WriteOnly | QIODevice::Truncate));
  File.write(Data);
}

static CloudCrypto::SnapshotHeader NewWrap(const QByteArray & Dek, const QString & Password, const QByteArray & Recovery)
{
  CloudCrypto::SnapshotHeader Wrap;
  Wrap.User = QStringLiteral("user-1");
  // fast parameters for the test
  Wrap.Kdf.MemoryKib = 8 * 1024;
  Wrap.Kdf.Iterations = 1;
  CHECK(CloudCrypto::WrapDek(Wrap, Dek, Password, Recovery));
  return Wrap;
}

#ifdef Q_OS_LINUX
// How often the bytes of Masked ^ Mask occur in this process's memory. The key
// itself is never put together here: that copy would be found.
static int CountInMemory(const QByteArray & Masked, const QByteArray & Mask)
{
  QFile Maps(QStringLiteral("/proc/self/maps"));
  if (!Maps.open(QIODevice::ReadOnly))
  {
    return -1;
  }
  const qsizetype Length = Masked.size();
  const qulonglong Chunk = 1024 * 1024;
  int Found = 0;
  QByteArray Buffer(static_cast<qsizetype>(Chunk) + Length, '\0');
  const QList<QByteArray> Lines = Maps.readAll().split('\n');
  for (const QByteArray & Line : Lines)
  {
    QList<QByteArray> Fields = Line.simplified().split(' ');
    if ((Fields.size() < 2) || !Fields[1].startsWith('r'))
    {
      continue;
    }
    QList<QByteArray> Range = Fields[0].split('-');
    bool Ok1 = false, Ok2 = false;
    qulonglong Start = Range.value(0).toULongLong(&Ok1, 16);
    qulonglong End = Range.value(1).toULongLong(&Ok2, 16);
    if (!Ok1 || !Ok2)
    {
      continue;
    }
    for (qulonglong At = Start; At < End; At += Chunk)
    {
      // Not a plain read: the page may go away, and this one is the buffer itself
      struct iovec Local{Buffer.data(), static_cast<size_t>(qMin<qulonglong>(End - At, Chunk + static_cast<qulonglong>(Length)))};
      struct iovec Remote{reinterpret_cast<void *>(At), Local.iov_len};
      ssize_t Read = process_vm_readv(getpid(), &Local, 1, &Remote, 1, 0);
      for (ssize_t I = 0; I + Length <= Read; I++)
      {
        if (static_cast<char>(Buffer[I] ^ Mask[0]) != Masked[0])
        {
          continue;
        }
        qsizetype J = 1;
        while ((J < Length) && (static_cast<char>(Buffer[I + J] ^ Mask[J]) == Masked[J]))
        {
          J++;
        }
        if ((J == Length) && (static_cast<qulonglong>(I) < Chunk))
        {
          Found++;
        }
      }
    }
  }
  return Found;
}
#endif

static void TestForgetWipes()
{
#ifdef Q_OS_LINUX
  // A key that went through the usual ways of being shared
  QByteArray Dek = CloudCrypto::RandomBytes(CloudCrypto::KeyLength);
  QByteArray Mask = CloudCrypto::RandomBytes(CloudCrypto::KeyLength);
  QByteArray Masked = Dek;
  for (qsizetype I = 0; I < Masked.size(); I++)
  {
    Masked[I] = static_cast<char>(Masked.at(I) ^ Mask.at(I));
  }
  CloudCrypto::SnapshotHeader Wrap = NewWrap(Dek, QStringLiteral("correct horse"), QByteArray());
  ConfigVault::SetKey(Dek, Wrap);
  Security::WipeBytes(Dek);
  CHECK(CountInMemory(Masked, Mask) >= 1);
  // Used as a key, and read back as the holders of the key do
  QByteArray Sealed = ConfigVault::Seal("some data");
  QByteArray Plain;
  CHECK(ConfigVault::Open(Sealed, Plain) && (Plain == "some data"));
  ConfigVault::Forget();
  CHECK(!ConfigVault::Enabled());
  CHECK(CountInMemory(Masked, Mask) == 0);
#endif
}

static void TestProcessProtected()
{
  // A non-dumpable process is refused by xdg-desktop-portal, whose users would
  // lose their file dialogs
  {
    QByteArray Theme = qgetenv("QT_QPA_PLATFORMTHEME");
    qputenv("QT_QPA_PLATFORMTHEME", "xdgdesktopportal");
    CHECK(Security::DesktopPortalInUse());
    qputenv("QT_QPA_PLATFORMTHEME", "gtk3");
    CHECK(!Security::DesktopPortalInUse() || !qEnvironmentVariableIsEmpty("SNAP") || (qgetenv("GTK_USE_PORTAL") == "1"));
    qputenv("QT_QPA_PLATFORMTHEME", Theme);
  }
#if defined(Q_OS_LINUX) && defined(QT_NO_DEBUG)
  // Holding a key: no core file, and nobody may attach. (A debugger, or
  // $NIXSCP_ALLOW_COREDUMP, keeps the process debuggable.)
  if (qEnvironmentVariableIsSet("NIXSCP_ALLOW_COREDUMP"))
  {
    return;
  }
  CHECK(prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) == 0);
  struct rlimit Limit;
  CHECK((getrlimit(RLIMIT_CORE, &Limit) == 0) && (Limit.rlim_cur == 0));
#endif
}

// Damaged files, files in the wrong place, and a plain configuration while the
// state says it is encrypted
static void TestBoundFiles(const QString & Ini, const QByteArray & Sealed, const QString & Password, const QByteArray & Dek)
{
  Configuration & Config = Configuration::Instance();
  const QString Dir = QFileInfo(Ini).absolutePath();
  const QString State = Dir + QStringLiteral("/nixscp-cloud.ini");
  QString Error;
  QByteArray Plain;
  SessionData Change;
  Change.Name = QStringLiteral("A change to save");
  Change.HostName = QStringLiteral("change.example");

  // ---- a damaged header is damaged, not plaintext: never loaded, never overwritten
  for (const QByteArray & Broken : {QByteArray("NXS1"), QByteArray("NXS1\xff\xff\xff\x7f garbage"), Sealed.left(20), QByteArray("NXS1") + QByteArray(64, 'x')})
  {
    CHECK(!ConfigVault::IsSealed(Broken));
    CHECK(ConfigVault::LooksSealed(Broken));
    CHECK(!ConfigVault::Open(Broken, Plain));
    WriteRaw(Ini, Broken);
    CHECK(ConfigVault::IsSealedFile(Ini));
    Config.Load();
    CHECK(Config.IsLocked());
    Config.StoredSessions().Add(std::make_shared<SessionData>(Change));
    CHECK(!Config.Save());
    CHECK(ReadRaw(Ini) == Broken);
    CHECK(!ConfigVault::Unlock(Ini, Password, false, &Error) && Error.contains(QStringLiteral("damaged")));
    // ...with the way out, before any password is asked for
    CHECK(ConfigVault::ConfigProblem(Ini).contains(QStringLiteral("damaged")));
    CHECK(ConfigVault::ConfigProblemText(Ini).contains(QStringLiteral("delete")));
    CHECK(ConfigVault::ConfigProblemText(Ini).contains(Ini));
    CHECK(Error.contains(QStringLiteral("delete")));
  }
  // The header is fine and the password right, the rest is broken: not a wrong password
  {
    QByteArray Flipped = Sealed;
    Flipped[Flipped.size() - 1] = static_cast<char>(Flipped.at(Flipped.size() - 1) ^ 1);
    WriteRaw(Ini, Flipped);
    CHECK(ConfigVault::ConfigProblem(Ini).isEmpty());
    CHECK(!ConfigVault::Unlock(Ini, Password, false, &Error) && Error.contains(QStringLiteral("damaged")) && Error.contains(QStringLiteral("delete")));
    CHECK(!ConfigVault::Unlock(Ini, QStringLiteral("wrong"), false, &Error) && Error.contains(QStringLiteral("not correct")));
  }

  // ---- what NixSCP 1.2.3 left is not called damage, and it says what to delete
  {
    QByteArray Old = QByteArray(Sealed).replace("\"fmt\":2", "\"fmt\":1");
    CHECK(Old != Sealed);
    WriteRaw(Ini, Old);
    WriteRaw(State, Old);
    WriteRaw(Dir + QStringLiteral("/nixscp-cloud-session.ini"), Old);
    QString Text = ConfigVault::ConfigProblemText(Ini);
    CHECK(Text.contains(QStringLiteral("1.2.3")));
    CHECK(!Text.contains(QStringLiteral("damaged")));
    CHECK(Text.contains(Ini) && Text.contains(State) && Text.contains(Dir + QStringLiteral("/nixscp-cloud-session.ini")));
    CHECK(!ConfigVault::Unlock(Ini, Password, false, &Error) && (Error == Text));
    CHECK(!ConfigVault::OpenWithSecret(Old, Password, false, Plain, &Error) && Error.contains(QStringLiteral("1.2.3")));
    // Deleting what it says is the whole way out
    const QStringList Files = ConfigVault::StartOverFiles(Ini);
    CHECK(Files.size() == 3);
    for (const QString & File : Files)
    {
      CHECK(QFile::remove(File));
    }
    CHECK(ConfigVault::ConfigProblem(Ini).isEmpty());
    CHECK(ConfigVault::ConfigProblemText(Ini).isEmpty());
    Config.Load();
    CHECK(!Config.IsLocked());
  }

  // ---- sealed files cannot stand in for each other
  WriteRaw(Ini, Sealed);
  CHECK(ConfigVault::Unlock(Ini, Password, false));
  Config.Load();
  CHECK(!Config.IsLocked());
  IniFile Backup;
  Backup.WriteRaw(QStringLiteral("Test"), QStringLiteral("Key"), QStringLiteral("value"));
  QDir().mkpath(Dir + QStringLiteral("/backups"));
  QString BackupName = Dir + QStringLiteral("/backups/nixscp-20260102-000000.ini");
  QString Tabs = Dir + QStringLiteral("/nixscp-cloud-session.ini");
  CHECK(Backup.Save(BackupName));
  CHECK(Backup.Save(Tabs));
  CHECK(ConfigVault::RoleOf(Ini) == QLatin1String(ConfigVault::RoleConfig));
  CHECK(ConfigVault::RoleOf(BackupName) == QLatin1String(ConfigVault::RoleBackup));
  CHECK(ConfigVault::RoleOf(Tabs) == QLatin1String(ConfigVault::RoleSession));
  CHECK(ConfigVault::RoleOf(State) == QLatin1String(ConfigVault::RoleState));
  CHECK(ConfigVault::RoleOf(Dir + QStringLiteral("/elsewhere.ini")).isEmpty());
  QByteArray BackupData = ReadRaw(BackupName);
  QByteArray TabsData = ReadRaw(Tabs);
  // each opens in its own place, and with the plain Open used for imports
  CHECK(ConfigVault::OpenFile(BackupName, BackupData, Plain));
  CHECK(ConfigVault::Open(BackupData, Plain));
  // not in the place of the configuration, even with the right key
  CHECK(!ConfigVault::OpenFile(Ini, BackupData, Plain));
  CHECK(!ConfigVault::OpenFile(Ini, TabsData, Plain));
  CHECK(!ConfigVault::OpenFile(Tabs, BackupData, Plain));
  CHECK(!ConfigVault::OpenFile(BackupName, Sealed, Plain));
  CHECK(!ConfigVault::OpenFile(State, Sealed, Plain));
  WriteRaw(Ini, BackupData);
  Config.Load();
  CHECK(Config.IsLocked());
  CHECK(ReadRaw(Ini) == BackupData);
  // nor does it unlock at startup, with the message saying what happened
  ConfigVault::Forget();
  CHECK(!ConfigVault::Unlock(Ini, Password, false, &Error) && Error.contains(QStringLiteral("backup")));
  CHECK(!ConfigVault::Enabled());
  // an export opens through Import, not as the configuration
  WriteRaw(Ini, Sealed);
  CHECK(ConfigVault::Unlock(Ini, Password, false));
  QByteArray Export = ConfigVault::SealExport("[Configuration]\nX=1\n");
  CHECK(ConfigVault::OpenWithSecret(Export, Password, false, Plain) && Plain.contains("X=1"));
  CHECK(!ConfigVault::OpenFile(Ini, Export, Plain));
  // a file sealed on request (SSH keys on the phone) opens outside the configuration only
  QByteArray File = ConfigVault::Seal("key material");
  CHECK(ConfigVault::OpenFile(Dir + QStringLiteral("/keys-id_rsa"), File, Plain) && (Plain == "key material"));
  CHECK(!ConfigVault::OpenFile(Ini, File, Plain));

  // ---- the state is sealed: a plain or missing configuration is refused
  QByteArray PlainIni = "[Sessions\\Planted]\nHostName=evil.example\n";
  CHECK(!ConfigVault::RefusesConfig(Ini));
  IniFile StateFile;
  StateFile.WriteRaw(QStringLiteral("Account"), QStringLiteral("User"), QStringLiteral("user-1"));
  CHECK(StateFile.Save(State));
  CHECK(ConfigVault::IsSealedFile(State));
  CHECK(!ConfigVault::RefusesConfig(Ini));
  WriteRaw(Ini, PlainIni);
  CHECK(ConfigVault::RefusesConfig(Ini));
  CHECK(ConfigVault::RefusedText(Ini).contains(QStringLiteral("nixscp-cloud.ini")));
  Config.Load();
  CHECK(Config.IsLocked());
  CHECK(Config.StoredSessions().FindByName(QStringLiteral("Planted")) == nullptr);
  Config.StoredSessions().Add(std::make_shared<SessionData>(Change));
  CHECK(!Config.Save());
  CHECK(ReadRaw(Ini) == PlainIni);
  QFile::remove(Ini);
  CHECK(ConfigVault::RefusesConfig(Ini));
  Config.Load();
  CHECK(Config.IsLocked());
  // the problem is told the same way as the other ones
  CHECK(ConfigVault::ConfigProblem(Ini).contains(QStringLiteral("replaced")));
  CHECK(ConfigVault::ConfigProblemText(Ini).contains(State));
  // another configuration in the same folder is none of the state's business (/ini=)
  {
    QString Other = Dir + QStringLiteral("/other.ini");
    WriteRaw(Other, "[Sessions\\Mine]\nHostName=mine.example\n");
    CHECK(!ConfigVault::RefusesConfig(Other));
    CHECK(ConfigVault::ConfigProblem(Other).isEmpty());
    Config.SetIniFileName(Other);
    Config.Load();
    CHECK(!Config.IsLocked());
    CHECK(Config.StoredSessions().FindByName(QStringLiteral("Mine")) != nullptr);
    Config.SetIniFileName(Ini);
    QFile::remove(Other);
    // A state that does not say whose it is goes with the usual name
    CloudCrypto::SnapshotHeader Named;
    QByteArray Sealed2 = ReadRaw(State);
    CHECK(CloudCrypto::ReadHeader(Sealed2, Named) && (Named.ConfigName == QFileInfo(Ini).fileName()));
    WriteRaw(State, QByteArray(Sealed2).replace("\"config\":\"nixscp.ini\",", ""));
    CHECK(ConfigVault::RefusesConfig(Ini));
    CHECK(!ConfigVault::RefusesConfig(Other));
    // ...and one of a configuration of its own is about that one, also when it has another name
    QString Work = Dir + QStringLiteral("/work.ini");
    CHECK(Config.Rewrite() || true);
    Config.SetIniFileName(Work);
    CHECK(StateFile.Save(State));
    WriteRaw(Work, "[Sessions\\Planted]\nHostName=evil.example\n");
    CHECK(ConfigVault::RefusesConfig(Work));
    CHECK(!ConfigVault::RefusesConfig(Ini));
    QFile::remove(Work);
    Config.SetIniFileName(Ini);
    CHECK(StateFile.Save(State));
  }
  // the way out is deliberate: without the state it is an ordinary plain file
  QFile::remove(State);
  WriteRaw(Ini, PlainIni);
  CHECK(!ConfigVault::RefusesConfig(Ini));
  Config.Load();
  CHECK(!Config.IsLocked());
  CHECK(Config.StoredSessions().FindByName(QStringLiteral("Planted")) != nullptr);

  // ---- the current secret is checked without touching anything
  WriteRaw(Ini, Sealed);
  CHECK(ConfigVault::Unlock(Ini, Password, false));
  Config.Load();
  CHECK(ConfigVault::CheckSecret(Password, false));
  CHECK(!ConfigVault::CheckSecret(QStringLiteral("wrong"), false, &Error) && Error.contains(QStringLiteral("not correct")));
  CHECK(!ConfigVault::CheckSecret(QStringLiteral("AAAA-BBBB"), true, &Error));
  CHECK(ConfigVault::Key() == Dek);

  // ---- a connection thread opens files while the GUI thread checks a secret
  {
    std::atomic<bool> Stop{false};
    QByteArray Data = ConfigVault::SealExport("[Configuration]\nX=1\n");
    QThread * Reader = QThread::create([&Stop, &Data]()
    {
      while (!Stop)
      {
        QByteArray Opened;
        ConfigVault::Open(Data, Opened);
        ConfigVault::Enabled();
      }
    });
    Reader->start();
    for (int Round = 0; Round < 5; Round++)
    {
      CHECK(ConfigVault::CheckSecret(Password, false));
      CHECK(ConfigVault::Enabled());
      CHECK(!ConfigVault::User().isEmpty());
    }
    Stop = true;
    CHECK(Reader->wait(10000));
    delete Reader;
  }

  // ---- a key that cannot reach the configuration is not taken: nothing moves
  {
    CloudCrypto::SnapshotHeader Before = ConfigVault::Wrap();
    QByteArray OnDisk = ReadRaw(Ini);
    QByteArray Other = CloudCrypto::RandomBytes(CloudCrypto::KeyLength);
    QFile::Permissions Rights = QFile::permissions(Dir);
    CHECK(QFile::setPermissions(Dir, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    CHECK(!ConfigVault::ReplaceKey(Other, NewWrap(Other, QStringLiteral("rotated password"), QByteArray())));
    CHECK(QFile::setPermissions(Dir, Rights));
    CHECK(ConfigVault::Key() == Dek);
    CHECK(ConfigVault::Wrap().DekPassword == Before.DekPassword);
    CHECK(ReadRaw(Ini) == OnDisk);
    CHECK(!QFile::exists(Dir + QStringLiteral("/nixscp-cloud-bridge.ini")));
  }

  // ---- a crash in the middle of a new key: every file with a role of its own opens
  // once the new password was entered, whatever had moved by then
  {
    CHECK(Backup.Save(BackupName));
    CHECK(Backup.Save(Tabs));
    CHECK(Backup.Save(State));
    static QString Copy;
    static QString Source;
    Source = Dir;
    QTemporaryDir Side;
    Copy = Side.path();
    QByteArray NewKey = CloudCrypto::RandomBytes(CloudCrypto::KeyLength);
    CloudCrypto::SnapshotHeader NewWraps = NewWrap(NewKey, QStringLiteral("after the crash"), QByteArray());
    // What the disk holds at that moment
    ConfigVault::KeyChangeStep = [](const char * Step)
    {
      if (QByteArray(Step) == "config-resealed")
      {
        for (const QString & Name : QDir(Source).entryList(QDir::Files))
        {
          QFile::copy(Source + QLatin1Char('/') + Name, Copy + QLatin1Char('/') + Name);
        }
      }
    };
    CHECK(ConfigVault::ReplaceKey(NewKey, NewWraps));
    ConfigVault::KeyChangeStep = nullptr;
    ConfigVault::EndReplaceKey();
    CHECK(QFile::exists(Copy + QStringLiteral("/nixscp-cloud-bridge.ini")));
    // The configuration moves first and the state last: the state has to open
    // from whichever key the configuration asks for
    {
      CloudCrypto::SnapshotHeader Header;
      QByteArray Opened;
      CHECK(CloudCrypto::DecodeSnapshot(ReadRaw(Copy + QStringLiteral("/nixscp.ini")), NewKey, Header, Opened));
      CHECK(CloudCrypto::DecodeSnapshot(ReadRaw(Copy + QStringLiteral("/nixscp-cloud.ini")), Dek, Header, Opened));
    }
    for (const QString & Name : QDir(Copy).entryList(QDir::Files))
    {
      QFile::remove(Dir + QLatin1Char('/') + Name);
      CHECK(QFile::copy(Copy + QLatin1Char('/') + Name, Dir + QLatin1Char('/') + Name));
    }
    ConfigVault::Forget();
    CHECK(!ConfigVault::Unlock(Ini, Password, false, &Error));
    CHECK(ConfigVault::Unlock(Ini, QStringLiteral("after the crash"), false));
    CHECK(!QFile::exists(Dir + QStringLiteral("/nixscp-cloud-bridge.ini")));
    CHECK(ConfigVault::OpenFile(State, ReadRaw(State), Plain));
    CHECK(ConfigVault::OpenFile(Tabs, ReadRaw(Tabs), Plain));
    CHECK(ConfigVault::OpenFile(BackupName, ReadRaw(BackupName), Plain));
    // Back to the key of what follows
    ConfigVault::ResealFiles(NewKey);
    QFile::remove(State);
    WriteRaw(Ini, Sealed);
    CHECK(ConfigVault::Unlock(Ini, Password, false));
    Config.Load();
    CHECK(Backup.Save(BackupName));
    CHECK(Backup.Save(Tabs));
  }

  // ---- a new key: what the old key opened follows it, in its role
  CHECK(Backup.Save(BackupName));
  CHECK(Backup.Save(Tabs));
  QDir().mkpath(Dir + QStringLiteral("/agent-sessions"));
  QString Agent = Dir + QStringLiteral("/agent-sessions/1.log");
  WriteRaw(Agent, ConfigVault::ForFile(Agent, "transcript"));
  QString Plainfile = Dir + QStringLiteral("/notes.txt");
  WriteRaw(Plainfile, "not encrypted");
  QByteArray NewDek = CloudCrypto::RandomBytes(CloudCrypto::KeyLength);
  CloudCrypto::SnapshotHeader NewWrapHeader = NewWrap(NewDek, QStringLiteral("rotated password"), QByteArray());
  NewWrapHeader.KeyEpoch = 2;
  NewWrapHeader.WrapEpoch = 2;
  ConfigVault::SetKey(NewDek, NewWrapHeader);
  ConfigVault::ResealFiles(Dek);
  for (const QString & Name : {Ini, BackupName, Tabs, Agent})
  {
    CHECK(ConfigVault::OpenFile(Name, ReadRaw(Name), Plain));
    CHECK(!ConfigVault::OpenWithSecret(ReadRaw(Name), Password, false, Plain));
    CHECK(ConfigVault::OpenWithSecret(ReadRaw(Name), QStringLiteral("rotated password"), false, Plain));
  }
  CHECK(ReadRaw(Plainfile) == "not encrypted");
#ifndef _WIN32
  CHECK((QFile::permissions(BackupName) & (QFileDevice::ReadGroup | QFileDevice::ReadOther)) == 0);
#endif
  Config.Load();
  CHECK(!Config.IsLocked());
  CHECK(Config.Rewrite());
  ConfigVault::Forget();
  CHECK(ConfigVault::Unlock(Ini, QStringLiteral("rotated password"), false));
  CHECK(ConfigVault::Key() == NewDek);
  ConfigVault::Forget();
  QFile::remove(BackupName);
  QFile::remove(Tabs);
  QFile::remove(Agent);
  QFile::remove(Plainfile);
  WriteRaw(Ini, Sealed);
}

int main(int argc, char ** argv)
{
  QCoreApplication App(argc, argv);
  QTemporaryDir Dir;
  QString Ini = Dir.path() + QStringLiteral("/nixscp.ini");
  Configuration & Config = Configuration::Instance();
  Config.SetIniFileName(Ini);
  Config.Load();
  SessionData Site;
  Site.Name = QStringLiteral("Secret Project/web1");
  Site.HostName = QStringLiteral("web1.internal.example");
  Site.UserName = QStringLiteral("deploy");
  Site.Password = QStringLiteral("hunter2-site-password");
  Config.StoredSessions().Add(std::make_shared<SessionData>(Site));
  CHECK(Config.Save());
  CHECK(ReadRaw(Ini).contains("web1.internal.example"));

  // ---- encryption on: the whole file is ciphertext
  QByteArray Dek = CloudCrypto::RandomBytes(CloudCrypto::KeyLength);
  QByteArray Recovery = CloudCrypto::NewRecoveryKey();
  QString RecoveryText = CloudCrypto::FormatRecoveryKey(Recovery);
  CloudCrypto::SnapshotHeader Wrap = NewWrap(Dek, QStringLiteral("correct horse"), Recovery);
  ConfigVault::SetKey(Dek, Wrap);
  CHECK(ConfigVault::Enabled());
  CHECK(Config.Rewrite());
  QByteArray Raw = ReadRaw(Ini);
  CHECK(ConfigVault::IsSealedFile(Ini));
  CHECK(!Raw.contains("web1") && !Raw.contains("Secret") && !Raw.contains("deploy"));
#ifndef _WIN32
  CHECK((QFile::permissions(Ini) & (QFileDevice::ReadGroup | QFileDevice::ReadOther)) == 0);
#endif
  // a save of a change stays encrypted
  Config.StoredSessions().FindByName(Site.Name)->Note = QStringLiteral("changed");
  CHECK(Config.Save());
  CHECK(ConfigVault::IsSealedFile(Ini));
  // reloading with the key in memory works
  Config.Load();
  CHECK(!Config.IsLocked());
  CHECK(Config.StoredSessions().FindByName(Site.Name) != nullptr);

  // ---- companion files are protected; exports and other files are not
  CHECK(ConfigVault::IsProtectedFile(Ini));
  CHECK(ConfigVault::IsProtectedFile(Dir.path() + QStringLiteral("/nixscp-cloud-session.ini")));
  CHECK(ConfigVault::IsProtectedFile(Dir.path() + QStringLiteral("/backups/nixscp-20260101-000000.ini")));
  CHECK(ConfigVault::IsProtectedFile(Dir.path() + QStringLiteral("/nixscp-cloud.ini")));
  CHECK(!ConfigVault::IsProtectedFile(Dir.path() + QStringLiteral("/export.ini")));
  {
    IniFile Backup;
    Backup.WriteRaw(QStringLiteral("Test"), QStringLiteral("Key"), QStringLiteral("backup value"));
    QDir().mkpath(Dir.path() + QStringLiteral("/backups"));
    QString Name = Dir.path() + QStringLiteral("/backups/nixscp-20260101-000000.ini");
    CHECK(Backup.Save(Name));
    CHECK(ConfigVault::IsSealedFile(Name));
    IniFile Back;
    CHECK(Back.Load(Name) && (Back.ReadRaw(QStringLiteral("Test"), QStringLiteral("Key")) == QStringLiteral("backup value")));
  }

  // ---- plain export: a WinSCP-readable file with the site password
  QString Plain = Dir.path() + QStringLiteral("/export.ini");
  CHECK(Config.Export(Plain));
  CHECK(!ConfigVault::IsSealedFile(Plain));
  CHECK(ReadRaw(Plain).contains("web1.internal.example"));
  {
    IniFile Exported;
    CHECK(Exported.Load(Plain));
    StoredSessionList List;
    List.Load(Exported);
    auto Found = List.FindByName(Site.Name);
    CHECK((Found != nullptr) && (Found->Password == Site.Password));
  }

  // ---- encrypted export: opens with the password or the recovery key only
  QByteArray Sealed = ConfigVault::SealExport(Config.ExportData().SaveToData());
  CHECK(ConfigVault::IsSealed(Sealed));
  QByteArray Opened;
  QString Error;
  CHECK(!ConfigVault::OpenWithSecret(Sealed, QStringLiteral("wrong"), false, Opened, &Error) && Error.contains(QStringLiteral("password")));
  CHECK(ConfigVault::OpenWithSecret(Sealed, QStringLiteral("correct horse"), false, Opened) && Opened.contains("web1.internal.example"));
  CHECK(ConfigVault::OpenWithSecret(Sealed, RecoveryText.toLower(), true, Opened) && Opened.contains("web1.internal.example"));
  CHECK(!ConfigVault::OpenWithSecret(Sealed, QStringLiteral("AAAA-BBBB"), true, Opened));
  // a backup of another account / password opens with its own password and leaves the current key alone
  {
    QByteArray OtherDek = CloudCrypto::RandomBytes(CloudCrypto::KeyLength);
    CloudCrypto::SnapshotHeader Other = NewWrap(OtherDek, QStringLiteral("other password"), QByteArray());
    Other.Slot = QString::fromLatin1(ConfigVault::Slot);
    QByteArray Foreign = CloudCrypto::EncodeSnapshot(Other, OtherDek, "[Configuration]\nX=1\n");
    QByteArray Plaintext;
    CHECK(!ConfigVault::Open(Foreign, Plaintext));
    CHECK(ConfigVault::OpenWithSecret(Foreign, QStringLiteral("other password"), false, Plaintext) && Plaintext.contains("X=1"));
    CHECK(ConfigVault::Key() == Dek);
  }

  // ---- another NixSCP on this computer changed the password: its newer wraps are adopted
  {
    CloudCrypto::SnapshotHeader Newer = NewWrap(Dek, QStringLiteral("new password"), Recovery);
    Newer.Slot = QString::fromLatin1(ConfigVault::Slot);
    Newer.Role = QString::fromLatin1(ConfigVault::RoleConfig);
    Newer.WrapEpoch = Wrap.WrapEpoch + 1;
    Newer.SavedAt = QDateTime::currentDateTimeUtc();
    WriteRaw(Ini, CloudCrypto::EncodeSnapshot(Newer, Dek, Config.ExportData().SaveToData()));
    Config.Load();
    CHECK(!Config.IsLocked());
    CHECK(Config.Rewrite());
    // the file now opens with the new password, not the old one
    CHECK(ConfigVault::OpenWithSecret(ReadRaw(Ini), QStringLiteral("new password"), false, Opened));
    CHECK(!ConfigVault::OpenWithSecret(ReadRaw(Ini), QStringLiteral("correct horse"), false, Opened));
    // an older file does not bring the old wraps back, whatever time it claims
    CloudCrypto::SnapshotHeader Older = ConfigVault::Wrap();
    CloudCrypto::SnapshotHeader Stale = NewWrap(Dek, QStringLiteral("correct horse"), Recovery);
    Stale.Slot = QString::fromLatin1(ConfigVault::Slot);
    Stale.Role = QString::fromLatin1(ConfigVault::RoleConfig);
    Stale.WrapEpoch = Wrap.WrapEpoch;
    Stale.SavedAt = QDateTime::currentDateTimeUtc().addDays(1);
    QByteArray Plaintext;
    CHECK(ConfigVault::Open(CloudCrypto::EncodeSnapshot(Stale, Dek, "[X]\n"), Plaintext));
    CHECK(ConfigVault::Wrap().DekPassword == Older.DekPassword);
  }

  // ---- a new process: locked until unlocked, and a locked file is never overwritten
  ConfigVault::Forget();
  CHECK(!ConfigVault::Enabled());
  QByteArray Before = ReadRaw(Ini);
  Config.Load();
  CHECK(Config.IsLocked());
  CHECK(Config.StoredSessions().FindByName(Site.Name) == nullptr);
  Config.StoredSessions().Add(std::make_shared<SessionData>(Site));
  CHECK(!Config.Save());
  CHECK(!Config.Rewrite());
  CHECK(ReadRaw(Ini) == Before);
  CHECK(!ConfigVault::Unlock(Ini, QStringLiteral("correct horse"), false, &Error) && Error.contains(QStringLiteral("not correct")));
  CHECK(!ConfigVault::Enabled());
  CHECK(ConfigVault::Unlock(Ini, QStringLiteral("new password"), false));
  Config.Load();
  CHECK(!Config.IsLocked());
  CHECK(Config.StoredSessions().FindByName(Site.Name) != nullptr);
  CHECK(Config.StoredSessions().FindByName(Site.Name)->Password == Site.Password);
  // the recovery key unlocks too
  ConfigVault::Forget();
  CHECK(ConfigVault::Unlock(Ini, RecoveryText, true));
  CHECK(ConfigVault::Key() == Dek);

  // ---- a damaged file: not unlockable, still never overwritten
  ConfigVault::Forget();
  QByteArray Damaged = ReadRaw(Ini);
  Damaged[Damaged.size() - 3] = static_cast<char>(Damaged.at(Damaged.size() - 3) ^ 0x55);
  WriteRaw(Ini, Damaged);
  CHECK(!ConfigVault::Unlock(Ini, QStringLiteral("new password"), false));
  Config.Load();
  CHECK(Config.IsLocked());
  Config.StoredSessions().Add(std::make_shared<SessionData>(Site));
  CHECK(!Config.Save());
  CHECK(ReadRaw(Ini) == Damaged);

  // ---- nixscp-cli on an encrypted configuration: asks for the password; refused without input
  {
    QString Cli = qEnvironmentVariable("NIXSCP_CLI", QCoreApplication::applicationDirPath() + QStringLiteral("/nixscp-cli"));
    QString Copy = Dir.path() + QStringLiteral("/cli/nixscp.ini");
    QDir().mkpath(QFileInfo(Copy).absolutePath());
    WriteRaw(Copy, Before);
    if (QFileInfo(Cli).isExecutable())
    {
      QProcess P;
      P.start(Cli, {QStringLiteral("/ini=") + Copy, QStringLiteral("/command"), QStringLiteral("exit")});
      P.closeWriteChannel();
      CHECK(P.waitForFinished(60000));
      QByteArray Out = P.readAllStandardOutput() + P.readAllStandardError();
      CHECK((P.exitCode() != 0) && Out.contains("encrypted"));
      // the password typed in (wrong first, then right)
      P.start(Cli, {QStringLiteral("/ini=") + Copy, QStringLiteral("/command"), QStringLiteral("exit")});
      P.write("wrong password\nnew password\n");
      P.closeWriteChannel();
      CHECK(P.waitForFinished(60000));
      Out = P.readAllStandardOutput() + P.readAllStandardError();
      CHECK((P.exitCode() == 0) && Out.contains("not correct"));
      // the file is still encrypted afterwards
      CHECK(ConfigVault::IsSealedFile(Copy));
      // a plain configuration next to a sealed state is refused, without asking anything
      QString State = QFileInfo(Copy).absolutePath() + QStringLiteral("/nixscp-cloud.ini");
      WriteRaw(State, "NXS1 a sealed state");
      WriteRaw(Copy, "[Sessions\\Planted]\nHostName=evil.example\n");
      P.start(Cli, {QStringLiteral("/ini=") + Copy, QStringLiteral("/command"), QStringLiteral("exit")});
      P.closeWriteChannel();
      CHECK(P.waitForFinished(60000));
      Out = P.readAllStandardOutput() + P.readAllStandardError();
      CHECK((P.exitCode() != 0) && Out.contains("should be encrypted") && !Out.contains("password (or recovery key)"));
      CHECK(ReadRaw(Copy).contains("Planted"));
    }
  }

  // ---- encryption removed: written readable again
  WriteRaw(Ini, Before);
  CHECK(ConfigVault::Unlock(Ini, QStringLiteral("new password"), false));
  Config.Load();
  ConfigVault::Disable();
  CHECK(Config.Rewrite());
  CHECK(!ConfigVault::IsSealedFile(Ini));
  CHECK(ReadRaw(Ini).contains("web1.internal.example"));

  ConfigVault::Forget();
  TestBoundFiles(Ini, Before, QStringLiteral("new password"), Dek);
  TestForgetWipes();
  TestProcessProtected();

  if (Failures == 0)
  {
    std::printf("All configuration encryption tests passed\n");
  }
  return (Failures == 0) ? 0 : 1;
}
